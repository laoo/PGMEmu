#include <catch2/catch_test_macros.hpp>

#include "support/Fixture.hpp"

#include "pgm/Emulator.hpp"
#include "pgm/control/Dispatcher.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using pgm::control::Dispatcher;
using pgm::control::Json;
using pgm::test::Fixture;

namespace
{

Json call( Dispatcher const& dispatcher, std::string const& method, Json params = Json::object() )
{
  return dispatcher.handle( Json{ { "id", 1 }, { "method", method }, { "params", std::move( params ) } } );
}

std::string errorCode( Json const& response )
{
  return response.at( "error" ).at( "code" ).get<std::string>();
}

} // namespace

TEST_CASE( "input.set and input.clear hold controls, as the simulator encodes them", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( errorCode( call( dispatcher, "input.set", { { "name", "start" } } ) ) == "not_loaded" );
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );

  REQUIRE( call( dispatcher, "input.set", { { "name", "start" } } ).at( "ok" ) == true );
  REQUIRE( call( dispatcher, "input.set", { { "name", "button1" } } ).at( "ok" ) == true );
  REQUIRE( call( dispatcher, "input.get_state" ).at( "result" ).at( "buttons" ) == 0x10010 );

  REQUIRE( call( dispatcher, "input.clear", { { "name", "start" } } ).at( "ok" ) == true );
  REQUIRE( call( dispatcher, "input.get_state" ).at( "result" ).at( "buttons" ) == 0x10 );

  REQUIRE( errorCode( call( dispatcher, "input.set", { { "name", "jump" } } ) ) == "invalid_input" );
}

TEST_CASE( "input.press holds a control for two frames and lets it go for two", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );

  auto const result = call( dispatcher, "input.press", { { "name", "up" } } ).at( "result" );

  REQUIRE( result.at( "frames_executed" ) == 4 );
  REQUIRE( call( dispatcher, "input.get_state" ).at( "result" ).at( "buttons" ) == 0 );
}

TEST_CASE( "input.press holds several controls for the frames it is told, beside those already held", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );
  REQUIRE( call( dispatcher, "input.set", { { "name", "start" } } ).at( "ok" ) == true );

  auto const result = call( dispatcher,
                            "input.press",
                            { { "buttons", { "down", "button2" } }, { "frames", 5 }, { "release_frames", 0 } } )
                          .at( "result" );

  REQUIRE( result.at( "reason" ) == "completed" );
  REQUIRE( result.at( "frames_executed" ) == 5 );
  // What was held before the press is held after it.
  REQUIRE( call( dispatcher, "input.get_state" ).at( "result" ).at( "held" ) == Json::array( { "start" } ) );

  REQUIRE( errorCode( call( dispatcher, "input.press", { { "buttons", { "down", "jump" } } } ) ) == "invalid_input" );
  REQUIRE( errorCode( call( dispatcher, "input.press", { { "buttons", { 1 } } } ) ) == "bad_request" );
  REQUIRE( errorCode( call( dispatcher, "input.press", { { "name", "up" }, { "frames", 0 } } ) ) == "bad_request" );
  REQUIRE( errorCode( call( dispatcher, "input.press", { { "name", "up" }, { "release_frames", 601 } } ) ) ==
           "bad_request" );
  REQUIRE( errorCode( call( dispatcher, "input.press" ) ) == "bad_request" );
}

TEST_CASE( "input.hold holds exactly the controls listed, and input.get_state names them", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( errorCode( call( dispatcher, "input.hold", { { "buttons", Json::array() } } ) ) == "not_loaded" );
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );
  REQUIRE( call( dispatcher, "input.set", { { "name", "coin" } } ).at( "ok" ) == true );

  // A control is named once, whichever of its names asked for it.
  auto const held = call( dispatcher, "input.hold", { { "buttons", { "start", "a", "btn1", "left" } } } );
  REQUIRE( held.at( "result" ).at( "held" ) == Json::array( { "left", "button1", "start" } ) );
  auto const state = call( dispatcher, "input.get_state" ).at( "result" );
  REQUIRE( state.at( "buttons" ) == 0x10012 );
  REQUIRE( state.at( "held" ) == held.at( "result" ).at( "held" ) );

  REQUIRE( call( dispatcher, "input.hold", { { "buttons", "coin" } } ).at( "result" ).at( "held" ) ==
           Json::array( { "coin" } ) );
  REQUIRE( call( dispatcher, "input.hold", { { "buttons", Json::array() } } ).at( "result" ).at( "held" ).empty() );
  REQUIRE( call( dispatcher, "input.get_state" ).at( "result" ).at( "buttons" ) == 0 );

  REQUIRE( errorCode( call( dispatcher, "input.hold", { { "buttons", { "jump" } } } ) ) == "invalid_input" );
  REQUIRE( errorCode( call( dispatcher, "input.hold" ) ) == "bad_request" );
}

TEST_CASE( "an audio capture is a WAV of what the runs between its start and stop produced", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );
  auto const path = ( fixture.romDirectory() / "capture.wav" ).string();

  REQUIRE( errorCode( call( dispatcher, "audio.capture_stop" ) ) == "capture_not_running" );
  REQUIRE( call( dispatcher, "audio.capture_start", { { "path", path } } ).at( "ok" ) == true );
  REQUIRE( errorCode( call( dispatcher, "audio.capture_start", { { "path", path } } ) ) == "capture_running" );
  REQUIRE( call( dispatcher, "emu.run_frames", { { "count", 2 } } ).at( "ok" ) == true );
  auto const stopped = call( dispatcher, "audio.capture_stop" ).at( "result" );

  // The filler BIOS never starts the ICS2115, so the WAV is empty, at the
  // rate of all 32 voices.
  REQUIRE( stopped.at( "frames" ) == 0 );
  REQUIRE( stopped.at( "sample_rate" ) == 33072 );
  std::ifstream file{ path, std::ios::binary };
  std::vector<char> const bytes{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
  REQUIRE( bytes.size() == 44 );
  REQUIRE( std::string( bytes.data(), 4 ) == "RIFF" );
  REQUIRE( std::string( bytes.data() + 8, 8 ) == "WAVEfmt " );
}

TEST_CASE( "cpu.get_state answers the Z80's registers for cpu z80", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );

  auto const state = call( dispatcher, "cpu.get_state", { { "cpu", "z80" } } ).at( "result" );

  // Powered up and never reset: every register zero, fetching from 0.
  REQUIRE( state.at( "pc" ) == 0 );
  REQUIRE( state.at( "sp" ) == 0 );
  REQUIRE( state.at( "halted" ) == false );
  REQUIRE( errorCode( call( dispatcher, "cpu.get_state", { { "cpu", "arm7" } } ) ) == "no_cpu" );
  REQUIRE( errorCode( call( dispatcher, "cpu.get_state", { { "cpu", "6502" } } ) ) == "bad_request" );
}

TEST_CASE( "state.save and state.load take the machine back to where it was saved", "[control]" )
{
  Fixture const fixture;
  pgm::Settings settings = fixture.settings();
  settings.stateDirectory = fixture.romDirectory();
  pgm::Emulator emulator{ settings };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( errorCode( call( dispatcher, "state.save", { { "filename", "a.pgmstate" } } ) ) == "not_loaded" );
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );
  REQUIRE( call( dispatcher, "emu.run_frames", { { "count", 3 } } ).at( "ok" ) == true );
  auto const savedAt = call( dispatcher, "emu.status" ).at( "result" ).at( "total_ticks" );

  REQUIRE( call( dispatcher, "state.save", { { "filename", "a.pgmstate" } } ).at( "ok" ) == true );
  REQUIRE( call( dispatcher, "state.list" ).at( "result" ).at( "states" ) == Json::array( { "a.pgmstate" } ) );
  REQUIRE( call( dispatcher, "emu.run_frames", { { "count", 2 } } ).at( "ok" ) == true );
  REQUIRE( call( dispatcher, "state.load", { { "filename", "a.pgmstate" } } ).at( "ok" ) == true );

  REQUIRE( call( dispatcher, "emu.status" ).at( "result" ).at( "total_ticks" ) == savedAt );

  // Another game's machine does not take it.
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "testcart" } } ).at( "ok" ) == true );
  REQUIRE( errorCode( call( dispatcher, "state.load", { { "filename", "a.pgmstate" } } ) ) == "state_mismatch" );
  REQUIRE( errorCode( call( dispatcher, "state.load", { { "filename", "none.pgmstate" } } ) ) == "state_failed" );
}

TEST_CASE( "nvram.save writes the work RAM as the RTL lays it out, and nvram.load reads it back", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );
  auto const path = ( fixture.romDirectory() / "pgm.nv" ).string();

  // Each word's low byte first.
  std::vector<char> bytes( 0x20000, 0 );
  bytes[0] = 0x34;
  bytes[1] = 0x12;
  {
    std::ofstream file{ path, std::ios::binary };
    file.write( bytes.data(), static_cast<std::streamsize>( bytes.size() ) );
  }
  REQUIRE( call( dispatcher, "nvram.load", { { "filename", path } } ).at( "ok" ) == true );
  REQUIRE( call( dispatcher, "memory.read", { { "region", "WORK_RAM" }, { "address", 0 }, { "size", 2 } } )
               .at( "result" )
               .at( "data_hex" ) == "1234" );

  auto const saved = ( fixture.romDirectory() / "again.nv" ).string();
  REQUIRE( call( dispatcher, "nvram.save", { { "filename", saved } } ).at( "ok" ) == true );
  std::ifstream file{ saved, std::ios::binary };
  std::vector<char> const written{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
  REQUIRE( written == bytes );
}
