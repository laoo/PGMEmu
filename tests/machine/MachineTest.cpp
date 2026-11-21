#include <catch2/catch_test_macros.hpp>

#include "support/Expect.hpp"
#include "support/Files.hpp"
#include "support/Program.hpp"

#include "pgm/cart/Bios.hpp"
#include "pgm/control/Dispatcher.hpp"
#include "pgm/machine/Machine.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using pgm::control::Json;
using pgm::machine::Machine;
using pgm::machine::StopReason;
using pgm::test::high;
using pgm::test::low;
using pgm::test::valueOf;

namespace
{

constexpr std::uint32_t STACK = 0x820000;
constexpr std::uint32_t ENTRY = 0x400;
constexpr std::uint32_t LOOP = 0x40e;
constexpr std::uint32_t VBLANK_HANDLER = 0x500;
constexpr std::uint32_t COUNTER = 0x800000;
constexpr std::uint32_t FLAGS = 0xb0e000;

// Interrupt 6 is autovectored: vector 24 + 6.
constexpr std::uint32_t LEVEL_6_AUTOVECTOR = 30;

/// A BIOS program that counts vertical blanks: it enables interrupt 6 and
/// waits, and the handler acknowledges the interrupt by clearing and setting
/// its enable, as PGM software does, and adds one to a word of work RAM.
pgm::test::Program vblankCounter()
{
  pgm::test::Program program{ STACK, ENTRY };
  program.vector( LEVEL_6_AUTOVECTOR, VBLANK_HANDLER );
  program.at( ENTRY,
              {
                  0x46fc,
                  0x2000, // move.w #$2000,sr
                  0x33fc,
                  0x0008,
                  high( FLAGS ),
                  low( FLAGS ), // move.w #8,$b0e000
                  0x4e71,       // nop
                  0x60fe,       // loop: bra.s loop
              } );
  program.at( VBLANK_HANDLER,
              {
                  0x33fc,
                  0x0000,
                  high( FLAGS ),
                  low( FLAGS ), // move.w #0,$b0e000
                  0x33fc,
                  0x0008,
                  high( FLAGS ),
                  low( FLAGS ), // move.w #8,$b0e000
                  0x5279,
                  high( COUNTER ),
                  low( COUNTER ), // addq.w #1,$800000
                  0x4e73,         // rte
              } );
  return program;
}

/// A BIOS made of `program` and empty tiles and samples, read from a scratch
/// directory as the emulator reads one.
class BiosFixture
{
public:
  explicit BiosFixture( pgm::test::Program const& program )
  {
    pgm::test::writeFile( mScratch.path() / "pgm_p02s.u20", program.bytes() );
    pgm::test::writeFile( mScratch.path() / "pgm_t01s.rom", std::vector<std::uint8_t>( 0x200000, 0 ) );
    pgm::test::writeFile( mScratch.path() / "pgm_m01s.rom", std::vector<std::uint8_t>( 0x200000, 0 ) );
  }

  [[nodiscard]] std::filesystem::path const& directory() const
  {
    return mScratch.path();
  }

  [[nodiscard]] pgm::cart::Bios load() const
  {
    std::array const places{ mScratch.path() };
    return valueOf( pgm::cart::Bios::load( valueOf( pgm::io::RomSources::open( places ) ) ) );
  }

private:
  pgm::test::TemporaryDirectory mScratch;
};

std::uint16_t wordAt( std::span<std::uint8_t const> ram, std::size_t at )
{
  return static_cast<std::uint16_t>( ( ram[at] << 8U ) | ram[at + 1] );
}

} // namespace

TEST_CASE( "the 68000 runs the BIOS and takes the vertical blank interrupt once a frame", "[machine]" )
{
  BiosFixture const fixture{ vblankCounter() };
  auto const bios = fixture.load();
  Machine machine{ bios, nullptr };
  machine.reset( 100 );

  auto const run = machine.runFrames( 5 );

  REQUIRE( run.reason == StopReason::COMPLETED );
  REQUIRE( run.frames == 5 );
  // A frame ends 11 master ticks after its interrupt is raised, too soon for
  // the 68000 to have taken it: five frames hold four handled interrupts.
  REQUIRE( wordAt( machine.workRam(), COUNTER & 0x1ffffU ) == 4 );
  REQUIRE( machine.m68kState().pc >= LOOP );
}

TEST_CASE( "frames are counted where the RTL simulator counts them", "[machine]" )
{
  BiosFixture const fixture{ vblankCounter() };
  auto const bios = fixture.load();
  Machine machine{ bios, nullptr };
  machine.reset( 100 );

  auto const run = machine.runFrames( 60 );

  // The simulator's sim.run_frames 60 after sim.reset 100 takes 50687911
  // master ticks; a run ends between instructions, a few ticks later.
  REQUIRE( run.ticks >= 50687911 );
  REQUIRE( run.ticks < 50687911 + 100 );
}

TEST_CASE( "a run stops at a breakpoint before the instruction, and the next run executes it", "[machine]" )
{
  BiosFixture const fixture{ vblankCounter() };
  auto const bios = fixture.load();
  Machine machine{ bios, nullptr };
  machine.addBreakpoint( VBLANK_HANDLER );

  auto const first = machine.runFrames( 3 );
  REQUIRE( first.reason == StopReason::BREAKPOINT );
  REQUIRE( machine.m68kState().pc == VBLANK_HANDLER );

  auto const second = machine.runTicks( 1000 );
  REQUIRE( second.reason == StopReason::COMPLETED );
  REQUIRE( wordAt( machine.workRam(), COUNTER & 0x1ffffU ) == 1 );
}

TEST_CASE( "run_until stops where its condition first holds", "[machine][control]" )
{
  BiosFixture const fixture{ vblankCounter() };
  pgm::Emulator emulator{ pgm::Settings{
      .biosSources = { fixture.directory() }, .romDirectory = {}, .stateDirectory = {} } };
  pgm::control::Dispatcher const dispatcher{ emulator };
  auto const call = [&]( std::string const& method, Json params )
  { return dispatcher.handle( Json{ { "id", 1 }, { "method", method }, { "params", std::move( params ) } } ); };
  REQUIRE( call( "sim.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );

  auto const run = call( "sim.run_until",
                         { { "condition", { { "type", "cpu_pc_equals" }, { "value", VBLANK_HANDLER } } },
                           { "timeout_cycles", 2000000 } } );

  REQUIRE( run.at( "result" ).at( "reason" ) == "condition_met" );
  REQUIRE( call( "cpu.get_state", Json::object() ).at( "result" ).at( "pc" ) == VBLANK_HANDLER );
}

TEST_CASE( "a run stops after the instruction that writes a watched address", "[machine]" )
{
  BiosFixture const fixture{ vblankCounter() };
  auto const bios = fixture.load();
  Machine machine{ bios, nullptr };
  machine.addWatchpoint( pgm::machine::Watchpoint{ .address = COUNTER + 1, .size = 1, .read = false, .write = true } );

  auto const run = machine.runFrames( 3 );

  REQUIRE( run.reason == StopReason::WATCHPOINT );
  auto const hit = valueOf( machine.watchpointHit() );
  REQUIRE( hit.write );
  REQUIRE( hit.address == COUNTER );
  REQUIRE( hit.value == 1 );
  REQUIRE( hit.pc == VBLANK_HANDLER + 16 );
  REQUIRE( wordAt( machine.workRam(), COUNTER & 0x1ffffU ) == 1 );

  // The addq reads the counter before it writes it; a read watchpoint sees the
  // read, and not the fetch of the instruction words around it.
  machine.removeWatchpoint( COUNTER + 1 );
  machine.addWatchpoint( pgm::machine::Watchpoint{ .address = COUNTER, .size = 2, .read = true, .write = false } );
  REQUIRE( machine.runFrames( 3 ).reason == StopReason::WATCHPOINT );
  auto const read = valueOf( machine.watchpointHit() );
  REQUIRE_FALSE( read.write );
  REQUIRE( read.value == 1 );

  machine.removeWatchpoint( COUNTER );
  REQUIRE( machine.watchpoints().empty() );
  REQUIRE( machine.runFrames( 3 ).reason == StopReason::COMPLETED );
}

TEST_CASE( "the trace holds the last instructions executed, oldest first", "[machine]" )
{
  BiosFixture const fixture{ vblankCounter() };
  auto const bios = fixture.load();
  Machine machine{ bios, nullptr };
  machine.addBreakpoint( VBLANK_HANDLER + 16 );
  machine.runFrames( 3 );

  auto const trace = machine.trace( 3 );

  REQUIRE( trace.size() == 3 );
  // The loop, then the handler up to the breakpoint.
  REQUIRE( trace[0].pc == LOOP );
  REQUIRE( trace[1].pc == VBLANK_HANDLER );
  REQUIRE( trace[2].pc == VBLANK_HANDLER + 8 );
  REQUIRE( trace[0].ticks < trace[1].ticks );
  REQUIRE( machine.trace( 100000 ).size() <= Machine::TRACE_SIZE );
}

TEST_CASE( "the trace keeps the registers before each instruction, and as many instructions as it is told",
           "[machine]" )
{
  BiosFixture const fixture{ vblankCounter() };
  auto const bios = fixture.load();
  Machine machine{ bios, nullptr };
  // The registers are kept only once asked for, and asking empties the trace.
  REQUIRE_FALSE( machine.traceRegisters() );
  machine.setTraceRegisters( true );
  REQUIRE( machine.step( 3, 1000000 ).reason == StopReason::COMPLETED );

  auto const trace = machine.trace( 10 );
  REQUIRE( trace.size() == 3 );
  REQUIRE( trace[0].pc == ENTRY );
  REQUIRE( trace[0].sr == 0x2700 );
  REQUIRE( trace[0].a[7] == STACK );
  // The first instruction lowered the interrupt mask.
  REQUIRE( trace[1].pc == ENTRY + 4 );
  REQUIRE( trace[1].sr == 0x2000 );
  REQUIRE( machine.traceAt( 0 ).pc == ENTRY + 12 );
  REQUIRE( machine.trace( 1, 1 ).front().pc == ENTRY + 4 );
  REQUIRE( machine.trace( 5, 3 ).empty() );

  // Told to keep fewer, it keeps the newest; told to keep more, it loses none.
  machine.setTraceSize( 2 );
  REQUIRE( machine.traceSize() == 2 );
  REQUIRE( machine.traceCount() == 2 );
  REQUIRE( machine.trace( 10 ).front().pc == ENTRY + 4 );
  machine.step( 1, 1000000 );
  REQUIRE( machine.trace( 10 ).front().pc == ENTRY + 12 );
  REQUIRE( machine.trace( 10 ).back().pc == LOOP );
  machine.setTraceSize( 8 );
  machine.step( 1, 1000000 );
  auto const longer = machine.trace( 10 );
  REQUIRE( longer.size() == 3 );
  REQUIRE( longer[0].pc == ENTRY + 12 );
  REQUIRE( longer[1].pc == LOOP );
  REQUIRE( longer[2].pc == LOOP );

  machine.setTraceRegisters( false );
  REQUIRE( machine.traceCount() == 0 );
  machine.step( 1, 1000000 );
  REQUIRE( machine.traceAt( 0 ).pc == LOOP );
  REQUIRE( machine.traceAt( 0 ).a[7] == 0 );
}

TEST_CASE( "a step runs the instructions it is asked for, and stops where any run would", "[machine]" )
{
  BiosFixture const fixture{ vblankCounter() };
  auto const bios = fixture.load();
  Machine machine{ bios, nullptr };

  REQUIRE( machine.step( 1, 1000000 ).reason == StopReason::COMPLETED );
  REQUIRE( machine.m68kState().pc == ENTRY + 4 );
  REQUIRE( machine.step( 2, 1000000 ).reason == StopReason::COMPLETED );
  REQUIRE( machine.m68kState().pc == LOOP );

  machine.addBreakpoint( VBLANK_HANDLER + 8 );
  REQUIRE( machine.step( 1000000, 100000000 ).reason == StopReason::BREAKPOINT );
  REQUIRE( machine.m68kState().pc == VBLANK_HANDLER + 8 );
  // The step from a breakpoint executes the instruction there.
  REQUIRE( machine.step( 1, 1000000 ).reason == StopReason::COMPLETED );
  REQUIRE( machine.m68kState().pc == VBLANK_HANDLER + 16 );
}

TEST_CASE( "a step of a 68000 nothing wakes gives up when its time is out", "[machine]" )
{
  pgm::test::Program program{ STACK, ENTRY };
  program.at( ENTRY, { 0x4e72, 0x2700 } ); // stop #$2700
  BiosFixture const fixture{ program };
  auto const bios = fixture.load();
  Machine machine{ bios, nullptr };

  auto const run = machine.step( 2, 2000000 );

  REQUIRE( run.reason == StopReason::TIMEOUT );
  REQUIRE( run.ticks >= 2000000 );
  REQUIRE( machine.m68kState().stopped );
  REQUIRE( machine.traceCount() == 1 );
}

TEST_CASE( "a debugger reads the 68000's address space, and writes it where RAM is mapped", "[machine]" )
{
  BiosFixture const fixture{ vblankCounter() };
  auto const bios = fixture.load();
  Machine machine{ bios, nullptr };
  std::array<std::uint8_t, 2> const bytes{ 0x12, 0x34 };

  REQUIRE( machine.writeBus( 0x810101, bytes ) );
  REQUIRE( wordAt( machine.workRam(), 0x10101 ) == 0x1234 );
  REQUIRE( machine.readBus( 0x810100, 4 ) == std::vector<std::uint8_t>{ 0, 0x12, 0x34, 0 } );
  // The program reads in the 68000's byte order, whichever its file keeps.
  REQUIRE( machine.readBus( ENTRY, 2 ) == std::vector<std::uint8_t>{ 0x46, 0xfc } );

  // IGS023 keeps the upper byte of a word of VRAM at the odd address.
  REQUIRE( machine.writeBus( 0x900000, bytes ) );
  REQUIRE( machine.videoRam()[1] == 0x12 );
  REQUIRE( machine.videoRam()[0] == 0x34 );
  REQUIRE( machine.readBus( 0x900000, 2 ) == std::vector<std::uint8_t>{ 0x12, 0x34 } );
  REQUIRE( machine.writeBus( 0xa00002, bytes ) );
  REQUIRE( wordAt( machine.paletteRam(), 2 ) == 0x1234 );
  REQUIRE( machine.readBus( 0xa00002, 2 ) == std::vector<std::uint8_t>{ 0x12, 0x34 } );

  // The ROM is not written, nor a range that leaves RAM for IGS023's registers.
  REQUIRE_FALSE( machine.writeBus( ENTRY, bytes ) );
  REQUIRE_FALSE( machine.writeBus( 0xafffff, bytes ) );
  REQUIRE( machine.paletteRam().back() == 0 );
  REQUIRE( machine.readBus( 0xfffffe, 8 ).size() == 2 );
}

TEST_CASE( "emu.step, debug.trace and debug.trace.find answer where the 68000 has been", "[machine][control]" )
{
  BiosFixture const fixture{ vblankCounter() };
  pgm::Emulator emulator{ pgm::Settings{
      .biosSources = { fixture.directory() }, .romDirectory = {}, .stateDirectory = {} } };
  pgm::control::Dispatcher const dispatcher{ emulator };
  auto const call = [&]( std::string const& method, Json params = Json::object() )
  { return dispatcher.handle( Json{ { "id", 1 }, { "method", method }, { "params", std::move( params ) } } ); };
  auto const result = [&]( std::string const& method, Json params = Json::object() )
  { return call( method, std::move( params ) ).at( "result" ); };
  auto const refused = [&]( std::string const& method, Json params )
  { return call( method, std::move( params ) ).at( "error" ).at( "code" ).get<std::string>(); };
  REQUIRE( call( "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );
  // Three frames of the loop are some hundred thousand instructions, and the
  // search below looks back to the first of them.
  REQUIRE( refused( "debug.trace", { { "registers", true } } ) == "registers_not_kept" );
  REQUIRE( refused( "debug.trace.find", { { "where", { { "d0", 0 } } } } ) == "registers_not_kept" );
  REQUIRE( result( "debug.trace.configure", { { "instructions", 200000 }, { "registers", true } } ) ==
           Json{ { "instructions", 200000 }, { "registers", true } } );

  auto const step = result( "emu.step", { { "count", 3 } } );
  REQUIRE( step.at( "reason" ) == "completed" );
  REQUIRE( step.at( "pc" ) == LOOP );
  REQUIRE( step.at( "disasm" ).get<std::string>().starts_with( "bra" ) );
  REQUIRE( result( "emu.step" ).at( "pc" ) == LOOP );
  REQUIRE( refused( "emu.step", { { "count", 0 } } ) == "bad_request" );

  auto const trace = result( "debug.trace", { { "count", 2 }, { "from", 1 }, { "registers", true } } );
  REQUIRE( trace.at( "total" ) == 4 );
  auto const& instructions = trace.at( "instructions" );
  REQUIRE( instructions.size() == 2 );
  REQUIRE( instructions[0].at( "index" ) == 2 );
  REQUIRE( instructions[0].at( "pc" ) == ENTRY + 4 );
  REQUIRE( instructions[0].at( "sr" ) == 0x2000 );
  REQUIRE( instructions[0].at( "a" ).at( 7 ) == STACK );
  REQUIRE( instructions[1].at( "index" ) == 1 );
  REQUIRE( instructions[1].at( "pc" ) == ENTRY + 12 );
  REQUIRE_FALSE( result( "debug.trace", { { "count", 1 } } ).at( "instructions" )[0].contains( "d" ) );

  REQUIRE( result( "emu.run_frames", { { "count", 3 } } ).at( "reason" ) == "completed" );

  // The handler's entry, every time it was taken: the newest matches, oldest first.
  auto const entries = result( "debug.trace.find", { { "pc", VBLANK_HANDLER } } );
  auto const& matches = entries.at( "matches" );
  REQUIRE( matches.size() >= 2 );
  REQUIRE( entries.at( "next_from" ).is_null() );
  REQUIRE( matches[0].at( "pc" ) == VBLANK_HANDLER );
  REQUIRE( matches[0].at( "index" ).get<std::uint64_t>() > matches[1].at( "index" ).get<std::uint64_t>() );

  // One at a time, the search goes on back from next_from.
  auto const newest = result( "debug.trace.find", { { "pc", VBLANK_HANDLER }, { "limit", 1 } } );
  REQUIRE( newest.at( "matches" ).size() == 1 );
  REQUIRE( newest.at( "matches" )[0] == matches.back() );
  auto const older =
      result( "debug.trace.find", { { "pc", VBLANK_HANDLER }, { "limit", 1 }, { "from", newest.at( "next_from" ) } } );
  REQUIRE( older.at( "matches" )[0] == matches[matches.size() - 2] );

  // By a register's value, within a range of addresses, and by what the instruction reads as.
  auto const first = result( "debug.trace.find",
                             { { "pc_start", ENTRY },
                               { "pc_end", ENTRY + 2 },
                               { "where", { { "sr", "0x2700" }, { "a7", STACK } } },
                               { "registers", true } } );
  REQUIRE( first.at( "matches" ).size() == 1 );
  REQUIRE( first.at( "matches" )[0].at( "index" ) == first.at( "total" ).get<std::uint64_t>() - 1 );
  REQUIRE( first.at( "matches" )[0].at( "sr" ) == 0x2700 );
  REQUIRE( result( "debug.trace.find", { { "pc", ENTRY }, { "where", { { "sr", 0 } } } } ).at( "matches" ).empty() );
  auto const returns = result( "debug.trace.find", { { "disasm_contains", "RTE" } } ).at( "matches" );
  REQUIRE( returns.size() == matches.size() );
  REQUIRE( returns[0].at( "pc" ) == VBLANK_HANDLER + 22 );

  REQUIRE( refused( "debug.trace.find", { { "pc", ENTRY }, { "pc_start", 0 }, { "pc_end", 2 } } ) == "bad_request" );
  REQUIRE( refused( "debug.trace.find", { { "pc_start", 4 }, { "pc_end", 2 } } ) == "bad_request" );
  REQUIRE( refused( "debug.trace.find", { { "where", { { "x9", 1 } } } } ) == "bad_request" );
  REQUIRE( refused( "debug.trace.find", { { "limit", 0 } } ) == "bad_request" );
}

TEST_CASE( "debug.trace.configure sets how many instructions the trace keeps, for this game and the next",
           "[machine][control]" )
{
  BiosFixture const fixture{ vblankCounter() };
  pgm::Emulator emulator{ pgm::Settings{
      .biosSources = { fixture.directory() }, .romDirectory = {}, .stateDirectory = {} } };
  pgm::control::Dispatcher const dispatcher{ emulator };
  auto const call = [&]( std::string const& method, Json params = Json::object() )
  { return dispatcher.handle( Json{ { "id", 1 }, { "method", method }, { "params", std::move( params ) } } ); };

  // Before any game is loaded, too.
  REQUIRE( call( "debug.trace.configure" ).at( "result" ) ==
           Json{ { "instructions", Machine::TRACE_SIZE }, { "registers", false } } );
  REQUIRE( call( "debug.trace.configure", { { "registers", 1 } } ).at( "error" ).at( "code" ) == "bad_request" );
  REQUIRE( call( "debug.trace.configure", { { "instructions", 2 } } ).at( "result" ).at( "instructions" ) == 2 );
  REQUIRE( call( "debug.trace.configure", { { "instructions", 0 } } ).at( "error" ).at( "code" ) == "bad_request" );
  REQUIRE(
      call( "debug.trace.configure", { { "instructions", Machine::MAX_TRACE_SIZE + 1 } } ).at( "error" ).at( "code" ) ==
      "bad_request" );

  REQUIRE( call( "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );
  REQUIRE( call( "emu.step", { { "count", 3 } } ).at( "ok" ) == true );
  auto const trace = call( "debug.trace" ).at( "result" );
  REQUIRE( trace.at( "total" ) == 2 );
  REQUIRE( trace.at( "instructions" )[0].at( "pc" ) == ENTRY + 4 );

  REQUIRE( call( "debug.trace.configure", { { "instructions", 100 }, { "registers", true } } ).at( "ok" ) == true );
  REQUIRE( call( "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );
  REQUIRE( call( "emu.step", { { "count", 3 } } ).at( "ok" ) == true );
  auto const kept = call( "debug.trace", { { "registers", true } } ).at( "result" );
  REQUIRE( kept.at( "total" ) == 3 );
  REQUIRE( kept.at( "instructions" )[0].at( "a" ).at( 7 ) == STACK );
  REQUIRE( call( "debug.trace.configure" ).at( "result" ) == Json{ { "instructions", 100 }, { "registers", true } } );
}

TEST_CASE( "run_until stops when memory holds a value", "[machine][control]" )
{
  BiosFixture const fixture{ vblankCounter() };
  pgm::Emulator emulator{ pgm::Settings{
      .biosSources = { fixture.directory() }, .romDirectory = {}, .stateDirectory = {} } };
  pgm::control::Dispatcher const dispatcher{ emulator };
  auto const until = [&]( Json condition )
  {
    return dispatcher.handle(
        Json{ { "id", 1 },
              { "method", "emu.run_until" },
              { "params", { { "condition", std::move( condition ) }, { "timeout_cycles", 5000000 } } } } );
  };
  REQUIRE( dispatcher.handle( Json{ { "id", 1 }, { "method", "emu.load_game" }, { "params", { { "name", "pgm" } } } } )
               .at( "ok" ) == true );

  // The counter's low byte, then the whole word.
  auto const byte = until( { { "type", "memory_equals" }, { "address", COUNTER + 1 }, { "value", 1 } } );
  REQUIRE( byte.at( "result" ).at( "reason" ) == "condition_met" );
  REQUIRE( wordAt( emulator.machine()->workRam(), COUNTER & 0x1ffffU ) == 1 );
  auto const word =
      until( { { "type", "memory_equals" }, { "address", COUNTER }, { "value", "0x0003" }, { "size", 2 } } );
  REQUIRE( word.at( "result" ).at( "reason" ) == "condition_met" );
  REQUIRE( wordAt( emulator.machine()->workRam(), COUNTER & 0x1ffffU ) == 3 );

  // A value it never holds, and sizes and addresses there are none of.
  REQUIRE( until( { { "type", "memory_equals" }, { "address", COUNTER }, { "value", 0xffff }, { "size", 2 } } )
               .at( "result" )
               .at( "reason" ) == "timeout" );
  REQUIRE( until( { { "type", "memory_equals" }, { "address", COUNTER }, { "value", 1 }, { "size", 3 } } )
               .at( "error" )
               .at( "code" ) == "bad_request" );
  REQUIRE( until( { { "type", "memory_equals" }, { "address", 0xffffff }, { "value", 1 }, { "size", 2 } } )
               .at( "error" )
               .at( "code" ) == "bad_request" );
}

TEST_CASE( "a breakpoint ends input.press where it is reached, the controls let go", "[machine][control]" )
{
  BiosFixture const fixture{ vblankCounter() };
  pgm::Emulator emulator{ pgm::Settings{
      .biosSources = { fixture.directory() }, .romDirectory = {}, .stateDirectory = {} } };
  pgm::control::Dispatcher const dispatcher{ emulator };
  auto const call = [&]( std::string const& method, Json params = Json::object() )
  { return dispatcher.handle( Json{ { "id", 1 }, { "method", method }, { "params", std::move( params ) } } ); };
  REQUIRE( call( "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );
  REQUIRE( call( "debug.breakpoint.add", { { "address", VBLANK_HANDLER } } ).at( "ok" ) == true );

  auto const press =
      call( "input.press", { { "buttons", { "up", "button1" } }, { "frames", 10 }, { "release_frames", 10 } } )
          .at( "result" );

  REQUIRE( press.at( "reason" ) == "breakpoint" );
  REQUIRE( press.at( "breakpoint" ).at( "address" ) == VBLANK_HANDLER );
  REQUIRE( press.at( "frames_executed" ).get<int>() < 10 );
  REQUIRE( call( "input.get_state" ).at( "result" ).at( "buttons" ) == 0 );
}

// The test BIOS leaves the Z80 out of reset with its RAM clear, so it runs
// NOPs through all of it: every Z80 address is an instruction it reaches.
TEST_CASE( "a Z80 breakpoint ends the run, with the Z80's registers as they were there", "[machine]" )
{
  BiosFixture const fixture{ vblankCounter() };
  auto const bios = fixture.load();
  Machine machine{ bios, nullptr };
  REQUIRE( machine.addBreakpoint( 0x100, pgm::machine::Cpu::Z80 ) );
  REQUIRE( machine.breakpoints( pgm::machine::Cpu::Z80 ).contains( 0x100 ) );
  REQUIRE( machine.hasDebugPoints() );

  // The first frame boundary comes 11 master ticks after power-up.
  auto const run = machine.runFrames( 2 );

  REQUIRE( run.reason == StopReason::BREAKPOINT );
  auto const hit = valueOf( machine.breakpointHit() );
  REQUIRE( hit.cpu == pgm::machine::Cpu::Z80 );
  REQUIRE( hit.address == 0x100 );
  REQUIRE( valueOf( hit.z80 ).pc == 0x100 );
  // The Z80 itself ran on to the 68000's time, a few NOPs further.
  REQUIRE( machine.z80Registers().pc > 0x100 );

  machine.removeBreakpoint( 0x100, pgm::machine::Cpu::Z80 );
  REQUIRE_FALSE( machine.hasDebugPoints() );
  REQUIRE( machine.runFrames( 3 ).reason == StopReason::COMPLETED );
  REQUIRE_FALSE( machine.breakpointHit().has_value() );
}

// Compared by what a machine shows rather than by its save state, whose
// bytes include structs' padding, which no run sets.
TEST_CASE( "stopping at Z80 breakpoints leaves the emulation as it would have been", "[machine]" )
{
  BiosFixture const fixture{ vblankCounter() };
  auto const bios = fixture.load();
  Machine plain{ bios, nullptr };
  Machine stopped{ bios, nullptr };
  stopped.addBreakpoint( 0x2000, pgm::machine::Cpu::Z80 );

  plain.runFrames( 4 );
  int stops = 0;
  while ( stopped.frame() < 4 )
  {
    if ( stopped.runFrames( 1 ).reason == StopReason::BREAKPOINT )
    {
      ++stops;
    }
  }

  REQUIRE( stops > 0 );
  REQUIRE( stopped.now() == plain.now() );
  REQUIRE( std::ranges::equal( stopped.workRam(), plain.workRam() ) );
  REQUIRE( std::ranges::equal( stopped.videoRam(), plain.videoRam() ) );
  REQUIRE( std::ranges::equal( stopped.paletteRam(), plain.paletteRam() ) );
  REQUIRE( std::ranges::equal( stopped.z80Ram(), plain.z80Ram() ) );
  REQUIRE( std::ranges::equal( stopped.picture(), plain.picture() ) );
  REQUIRE( stopped.m68kState().pc == plain.m68kState().pc );
  REQUIRE( stopped.m68kState().d == plain.m68kState().d );
  REQUIRE( stopped.m68kState().a == plain.m68kState().a );
  REQUIRE( stopped.m68kState().sr == plain.m68kState().sr );
  REQUIRE( stopped.z80Registers().pc == plain.z80Registers().pc );
  REQUIRE( stopped.z80Registers().af == plain.z80Registers().af );
  REQUIRE( stopped.z80Registers().sp == plain.z80Registers().sp );
}

TEST_CASE( "the board without an IGS027A takes no ARM7 breakpoint", "[machine]" )
{
  BiosFixture const fixture{ vblankCounter() };
  auto const bios = fixture.load();
  Machine machine{ bios, nullptr };

  REQUIRE_FALSE( machine.addBreakpoint( 0x1000, pgm::machine::Cpu::ARM7 ) );
  REQUIRE( machine.breakpoints( pgm::machine::Cpu::ARM7 ).empty() );
}

TEST_CASE( "a debugger writes RAM whole or not at all", "[machine]" )
{
  BiosFixture const fixture{ vblankCounter() };
  auto const bios = fixture.load();
  Machine machine{ bios, nullptr };
  std::array<std::uint8_t, 2> const bytes{ 0x12, 0x34 };

  REQUIRE( machine.writeRam( pgm::machine::Ram::WORK, 0x100, bytes ) );
  REQUIRE( wordAt( machine.workRam(), 0x100 ) == 0x1234 );
  REQUIRE( machine.writeRam( pgm::machine::Ram::AUDIO, 0xfffe, bytes ) );
  REQUIRE( wordAt( machine.z80Ram(), 0xfffe ) == 0x1234 );
  REQUIRE( machine.writeRam( pgm::machine::Ram::PALETTE, 0, bytes ) );
  REQUIRE( wordAt( machine.paletteRam(), 0 ) == 0x1234 );

  REQUIRE_FALSE( machine.writeRam( pgm::machine::Ram::VIDEO, machine.videoRam().size() - 1, bytes ) );
  REQUIRE( machine.videoRam().back() == 0 );
}
