#include <catch2/catch_test_macros.hpp>

#include "support/Files.hpp"
#include "support/Fixture.hpp"
#include "support/PgmFile.hpp"

#include "pgm/Emulator.hpp"
#include "pgm/control/Dispatcher.hpp"

#include <algorithm>
#include <filesystem>
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

TEST_CASE( "a game is loaded by its set name from the ROM directory", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };

  REQUIRE( call( dispatcher, "emu.status" ).at( "result" ).at( "game_name" ).is_null() );

  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "testcart" } } ).at( "ok" ) == true );

  REQUIRE( call( dispatcher, "emu.status" ).at( "result" ).at( "game_name" ) == "testcart" );
  auto const info = call( dispatcher, "emu.cartridge_info" ).at( "result" );
  REQUIRE( info.at( "short_name" ) == "testcart" );
  REQUIRE( info.at( "roms" ).at( 0 ).at( "type" ) == "PRG" );
  REQUIRE( info.at( "roms" ).at( 0 ).at( "mapping" ) == 0x100000 );
}

TEST_CASE( "a game is loaded from a path, and sim.load_game is its alias", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  auto const path = ( fixture.romDirectory() / "testcart.pgm" ).string();

  REQUIRE( call( dispatcher, "sim.load_game", { { "path", path } } ).at( "ok" ) == true );
  REQUIRE( emulator.gameName() == "testcart" );
}

TEST_CASE( "pgm loads the BIOS alone", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };

  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );

  REQUIRE( call( dispatcher, "emu.status" ).at( "result" ).at( "game_name" ) == "pgm" );
  REQUIRE( errorCode( call( dispatcher, "emu.cartridge_info" ) ) == "no_cartridge" );
  REQUIRE( call( dispatcher, "memory.list_regions" ).at( "result" ) == Json::array( { "BIOS_PROG_ROM",
                                                                                      "BIOS_TILE_ROM",
                                                                                      "BIOS_MUSIC_ROM",
                                                                                      "WORK_RAM",
                                                                                      "VIDEO_RAM",
                                                                                      "PALETTE_RAM",
                                                                                      "AUDIO_RAM",
                                                                                      "M68K" } ) );
}

TEST_CASE( "a load that fails says why, and leaves what was loaded", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "testcart" } } ).at( "ok" ) == true );

  SECTION( "a set that is not in the ROM directory" )
  {
    REQUIRE( errorCode( call( dispatcher, "emu.load_game", { { "name", "nosuchgame" } } ) ) == "unknown_game" );
  }

  SECTION( "a name that is a path" )
  {
    REQUIRE( errorCode( call( dispatcher, "emu.load_game", { { "name", "../roms/testcart" } } ) ) == "unknown_game" );
  }

  SECTION( "a file that is not a .pgm" )
  {
    auto const path = ( fixture.biosDirectory() / "pgm_p02s.u20" ).string();
    REQUIRE( errorCode( call( dispatcher, "emu.load_game", { { "path", path } } ) ) == "load_failed" );
  }

  SECTION( "both a name and a path, or neither" )
  {
    REQUIRE( errorCode( call( dispatcher, "emu.load_game", { { "name", "a" }, { "path", "b" } } ) ) == "bad_request" );
    REQUIRE( errorCode( call( dispatcher, "emu.load_game" ) ) == "bad_request" );
  }

  REQUIRE( emulator.gameName() == "testcart" );
}

TEST_CASE( "a game runs as its image's region, or as the one it is loaded with", "[control]" )
{
  Fixture const fixture;
  pgm::test::PgmFile cart;
  cart.shortName = "regioncart";
  cart.hardware = 1;
  cart.roms = { pgm::test::PgmRom{ .type = 1, .mapping = 0x100000, .data = { 0x4e, 0x71, 0x4e, 0x75 } } };
  cart.regionBlock =
      pgm::test::asic3RegionBlock( 3, { { pgm::test::fourCc( "WRLD" ), 0 }, { pgm::test::fourCc( "CHNA" ), 3 } } );
  pgm::test::writeFile( fixture.romDirectory() / "regioncart.pgm", pgm::test::write( cart ) );
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  auto const region = [&] { return call( dispatcher, "emu.status" ).at( "result" ).at( "region" ); };

  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "testcart" } } ).at( "ok" ) == true );
  REQUIRE( region().is_null() );

  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "regioncart" } } ).at( "ok" ) == true );
  REQUIRE( region() == "CHNA" );

  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "regioncart" }, { "region", "WRLD" } } ).at( "ok" ) == true );
  REQUIRE( region() == "WRLD" );

  auto const refused = call( dispatcher, "emu.load_game", { { "name", "regioncart" }, { "region", "KREA" } } );
  REQUIRE( errorCode( refused ) == "unknown_region" );
  REQUIRE( refused.at( "error" ).at( "message" ).get<std::string>().contains( "it has WRLD, CHNA" ) );
  REQUIRE( region() == "WRLD" );

  REQUIRE( call( dispatcher, "emu.run_frames", { { "count", 2 } } ).at( "ok" ) == true );
  REQUIRE( call( dispatcher, "emu.set_region", { { "region", "CHNA" } } ).at( "ok" ) == true );
  REQUIRE( region() == "CHNA" );
  REQUIRE( call( dispatcher, "emu.status" ).at( "result" ).at( "frame" ) == 0 ); // powered up again
  REQUIRE( errorCode( call( dispatcher, "emu.set_region", { { "region", "KREA" } } ) ) == "unknown_region" );
  REQUIRE( region() == "CHNA" );

  REQUIRE( errorCode( call( dispatcher, "emu.load_game", { { "name", "testcart" }, { "region", "WRLD" } } ) ) ==
           "unknown_region" );
  REQUIRE( errorCode( call( dispatcher, "emu.load_game", { { "name", "pgm" }, { "region", "WRLD" } } ) ) ==
           "unknown_region" );
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );
  REQUIRE( errorCode( call( dispatcher, "emu.set_region", { { "region", "WRLD" } } ) ) == "unknown_region" );
  REQUIRE( errorCode( call( dispatcher, "emu.load_game", { { "name", "regioncart" }, { "region", 5 } } ) ) ==
           "bad_request" );
}

TEST_CASE( "emu.set_bios says where the BIOS is for the loads that follow", "[control]" )
{
  Fixture const fixture;
  pgm::Settings settings = fixture.settings();
  settings.biosSources.clear();
  pgm::Emulator emulator{ settings };
  Dispatcher const dispatcher{ emulator };

  REQUIRE( errorCode( call( dispatcher, "emu.load_game", { { "name", "testcart" } } ) ) == "load_failed" );
  REQUIRE( errorCode( call( dispatcher, "emu.set_bios", { { "sources", Json::array( { "/no/such/pgm.zip" } ) } } ) ) ==
           "bad_request" );
  REQUIRE( errorCode( call( dispatcher, "emu.set_bios", { { "sources", Json::array() } } ) ) == "bad_request" );

  REQUIRE( call( dispatcher, "emu.set_bios", { { "sources", Json::array( { fixture.biosDirectory().string() } ) } } )
               .at( "ok" ) == true );
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "testcart" } } ).at( "ok" ) == true );
}

TEST_CASE( "no BIOS source means no game can be loaded", "[control]" )
{
  Fixture const fixture;
  pgm::Settings settings = fixture.settings();
  settings.biosSources.clear();
  pgm::Emulator emulator{ settings };
  Dispatcher const dispatcher{ emulator };

  auto const response = call( dispatcher, "emu.load_game", { { "name", "testcart" } } );

  REQUIRE( errorCode( response ) == "load_failed" );
  REQUIRE( response.at( "error" ).at( "message" ).get<std::string>().contains( "BIOS" ) );
}

TEST_CASE( "memory.read answers bytes of a region as lowercase hex", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "testcart" } } ).at( "ok" ) == true );

  auto const read =
      call( dispatcher, "memory.read", { { "region", "CART_PROG_ROM" }, { "address", 0 }, { "size", 4 } } );

  REQUIRE( read.at( "result" ) == Json{ { "region", "CART_PROG_ROM" }, { "address", 0 }, { "data_hex", "4e714e75" } } );
  REQUIRE( call( dispatcher, "memory.read", { { "region", "BIOS_TILE_ROM" }, { "address", 0x1ffffe }, { "size", 2 } } )
               .at( "result" )
               .at( "data_hex" ) == "b1b1" );
}

TEST_CASE( "memory.read refuses what it cannot read", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "testcart" } } ).at( "ok" ) == true );

  auto const read = [&]( std::string const& region, std::uint64_t address, std::uint64_t size )
  { return call( dispatcher, "memory.read", { { "region", region }, { "address", address }, { "size", size } } ); };

  REQUIRE( errorCode( read( "CART_ARM_ROM", 0, 1 ) ) == "invalid_region" );
  REQUIRE( errorCode( read( "CART_PROG_ROM", 2, 3 ) ) == "invalid_range" );
  REQUIRE( errorCode( read( "CART_PROG_ROM", 5, 0 ) ) == "invalid_range" );
  REQUIRE( errorCode( read( "BIOS_TILE_ROM", 0, 0x100001 ) ) == "bad_request" );
  REQUIRE( read( "CART_PROG_ROM", 4, 0 ).at( "result" ).at( "data_hex" ).get<std::string>().empty() );
}

TEST_CASE( "memory.write writes RAM as memory.read reads it, addresses in hex too", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "testcart" } } ).at( "ok" ) == true );

  auto const written = call(
      dispatcher, "memory.write", { { "region", "WORK_RAM" }, { "address", "0x100" }, { "data_hex", "DEADbeef" } } );
  REQUIRE( written.at( "result" ) == Json{ { "region", "WORK_RAM" }, { "address", 0x100 }, { "size", 4 } } );
  auto const read =
      call( dispatcher, "memory.read", { { "region", "WORK_RAM" }, { "address", "$100" }, { "size", 4 } } );
  REQUIRE( read.at( "result" ).at( "data_hex" ) == "deadbeef" );

  REQUIRE( errorCode( call( dispatcher,
                            "memory.write",
                            { { "region", "CART_PROG_ROM" }, { "address", 0 }, { "data_hex", "00" } } ) ) ==
           "invalid_region" );
  REQUIRE( errorCode( call( dispatcher,
                            "memory.write",
                            { { "region", "WORK_RAM" }, { "address", 0x1ffff }, { "data_hex", "0000" } } ) ) ==
           "invalid_range" );
  REQUIRE( errorCode( call(
               dispatcher, "memory.write", { { "region", "WORK_RAM" }, { "address", 0 }, { "data_hex", "0x00" } } ) ) ==
           "bad_request" );
  REQUIRE( errorCode(
               call( dispatcher, "memory.read", { { "region", "WORK_RAM" }, { "address", "0x" }, { "size", 1 } } ) ) ==
           "bad_request" );
}

TEST_CASE( "without a region, memory is read and written by the 68000's addresses", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( errorCode( call( dispatcher, "memory.read", { { "address", 0 }, { "size", 1 } } ) ) == "not_loaded" );
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "testcart" } } ).at( "ok" ) == true );
  auto const regions = call( dispatcher, "memory.list_regions" ).at( "result" );
  REQUIRE( std::ranges::find( regions, "M68K" ) != regions.end() );

  // The cartridge's program at its mapping, each word as the 68000 reads it:
  // the file keeps the low byte first.
  auto const program = call( dispatcher, "memory.read", { { "address", "0x100000" }, { "size", 4 } } );
  REQUIRE( program.at( "result" ) ==
           Json{ { "region", "M68K" }, { "address", 0x100000 }, { "data_hex", "714e754e" } } );

  auto const written = call( dispatcher, "memory.write", { { "address", "0x810100" }, { "data_hex", "c0ffee" } } );
  REQUIRE( written.at( "result" ) == Json{ { "region", "M68K" }, { "address", 0x810100 }, { "size", 3 } } );
  REQUIRE( call( dispatcher, "memory.read", { { "region", "WORK_RAM" }, { "address", 0x10100 }, { "size", 3 } } )
               .at( "result" )
               .at( "data_hex" ) == "c0ffee" );
  REQUIRE( call( dispatcher, "memory.read", { { "region", "M68K" }, { "address", 0x810100 }, { "size", 3 } } )
               .at( "result" )
               .at( "data_hex" ) == "c0ffee" );

  // The ROM is not RAM, and the space ends at 16 MB.
  REQUIRE( errorCode( call( dispatcher, "memory.write", { { "address", 0x100000 }, { "data_hex", "00" } } ) ) ==
           "invalid_range" );
  REQUIRE( errorCode( call( dispatcher, "memory.read", { { "address", 0xffffff }, { "size", 2 } } ) ) ==
           "invalid_range" );
  REQUIRE( call( dispatcher, "memory.read", { { "address", 0xffffff }, { "size", 1 } } ).at( "ok" ) == true );
}

TEST_CASE( "emu.pause and emu.resume set what emu.status says of the window's own run", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };

  REQUIRE( call( dispatcher, "emu.status" ).at( "result" ).at( "paused" ) == false );
  REQUIRE( call( dispatcher, "emu.pause" ).at( "ok" ) == true );
  REQUIRE( emulator.paused() );
  REQUIRE( call( dispatcher, "emu.status" ).at( "result" ).at( "paused" ) == true );
  REQUIRE( call( dispatcher, "emu.resume" ).at( "ok" ) == true );
  REQUIRE_FALSE( emulator.paused() );
}

TEST_CASE( "breakpoints are set on a CPU the board has, and a run's answer says which stopped it", "[control]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "testcart" } } ).at( "ok" ) == true );

  REQUIRE( errorCode( call( dispatcher, "debug.breakpoint.add", { { "address", 0 }, { "cpu", "arm7" } } ) ) ==
           "no_cpu" );
  REQUIRE( errorCode( call( dispatcher, "debug.breakpoint.add", { { "address", 0 }, { "cpu", "gpu" } } ) ) ==
           "bad_request" );
  REQUIRE( call( dispatcher, "debug.breakpoint.add", { { "address", "0x80" }, { "cpu", "z80" } } ).at( "ok" ) == true );
  REQUIRE( call( dispatcher, "debug.breakpoint.list", { { "cpu", "z80" } } ).at( "result" ) ==
           Json::array( { 0x80 } ) );
  REQUIRE( call( dispatcher, "debug.breakpoint.list" ).at( "result" ) == Json::array() );

  auto const run = call( dispatcher, "emu.run_frames", { { "count", 2 } } ).at( "result" );
  REQUIRE( run.at( "reason" ) == "breakpoint" );
  REQUIRE( run.at( "breakpoint" ).at( "cpu" ) == "z80" );
  REQUIRE( run.at( "breakpoint" ).at( "address" ) == 0x80 );
  REQUIRE( run.at( "breakpoint" ).at( "registers" ).at( "pc" ) == 0x80 );
  REQUIRE( call( dispatcher, "emu.status" ).at( "result" ).at( "breakpoint" ) == run.at( "breakpoint" ) );
}
