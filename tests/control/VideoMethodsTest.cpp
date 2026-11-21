#include <catch2/catch_test_macros.hpp>

#include "support/Fixture.hpp"

#include "pgm/Emulator.hpp"
#include "pgm/control/Dispatcher.hpp"

#include <algorithm>
#include <string>

using pgm::control::Dispatcher;
using pgm::control::Json;
using pgm::test::Fixture;

namespace
{

Json call( Dispatcher const& dispatcher, std::string const& method, Json params = Json::object() )
{
  return dispatcher.handle( Json{ { "id", 1 }, { "method", method }, { "params", std::move( params ) } } );
}

/// The decoded length of base64 text.
std::size_t base64Bytes( std::string const& text )
{
  auto const padding = static_cast<std::size_t>( std::ranges::count( text, '=' ) );
  return ( ( text.size() / 4 ) * 3 ) - padding;
}

} // namespace

TEST_CASE( "video.layers turns layers off for the picture, and answers all three", "[control][video]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( call( dispatcher, "video.layers" ).at( "error" ).at( "code" ) == "not_loaded" );
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );

  auto const all = call( dispatcher, "video.layers" ).at( "result" );
  REQUIRE( all == Json{ { "text", true }, { "background", true }, { "sprites", true } } );

  auto const changed = call( dispatcher, "video.layers", { { "background", false } } ).at( "result" );
  REQUIRE( changed == Json{ { "text", true }, { "background", false }, { "sprites", true } } );
  REQUIRE( call( dispatcher, "video.layers", { { "text", 1 } } ).at( "error" ).at( "code" ) == "bad_request" );
}

TEST_CASE( "video.screenshot makes each pixel as many pixels as it is told", "[control][video]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );

  auto const plain = call( dispatcher, "video.screenshot" ).at( "result" );
  REQUIRE( plain.at( "width" ) == 448 );
  REQUIRE( plain.at( "height" ) == 224 );
  REQUIRE( plain.at( "scale" ) == 1 );

  auto const large = call( dispatcher, "video.screenshot", { { "scale", 3 } } ).at( "result" );
  REQUIRE( large.at( "width" ) == 1344 );
  REQUIRE( large.at( "height" ) == 672 );
  REQUIRE( large.at( "scale" ) == 3 );
  REQUIRE( large.at( "png_base64" ).get<std::string>().starts_with( "iVBORw0KGgo" ) );

  REQUIRE( call( dispatcher, "video.screenshot", { { "scale", 0 } } ).at( "error" ).at( "code" ) == "bad_request" );
  REQUIRE( call( dispatcher, "video.screenshot", { { "scale", 9 } } ).at( "error" ).at( "code" ) == "bad_request" );
}

TEST_CASE( "video.registers and video.sprites describe IGS023's state", "[control][video]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );

  auto const registers = call( dispatcher, "video.registers" ).at( "result" );
  REQUIRE( registers.at( "registers" ).size() == 16 );
  REQUIRE( registers.at( "zoom_table" ).size() == 32 );
  REQUIRE( registers.contains( "background_scroll" ) );

  auto const sprites = call( dispatcher, "video.sprites" ).at( "result" );
  REQUIRE( sprites.at( "count" ) == 0 );
  REQUIRE( sprites.at( "sprites" ).empty() );
}

TEST_CASE( "video.tiles and video.tilemap answer images of the sizes their layers give", "[control][video]" )
{
  Fixture const fixture;
  pgm::Emulator emulator{ fixture.settings() };
  Dispatcher const dispatcher{ emulator };
  REQUIRE( call( dispatcher, "emu.load_game", { { "name", "pgm" } } ).at( "ok" ) == true );

  auto const text = call( dispatcher, "video.tiles", { { "layer", "text" }, { "count", 20 }, { "columns", 8 } } );
  REQUIRE( text.at( "result" ).at( "width" ) == 64 );
  REQUIRE( text.at( "result" ).at( "height" ) == 24 );
  REQUIRE( text.at( "result" ).at( "png_base64" ).get<std::string>().starts_with( "iVBORw0KGgo" ) );

  auto const background =
      call( dispatcher, "video.tiles", { { "layer", "background" }, { "count", 2 }, { "format", "rgba" } } );
  REQUIRE( background.at( "result" ).at( "width" ) == 64 );
  REQUIRE( background.at( "result" ).at( "height" ) == 32 );
  REQUIRE( base64Bytes( background.at( "result" ).at( "rgba_base64" ).get<std::string>() ) ==
           std::size_t{ 64 } * 32 * 4 );

  auto const map = call( dispatcher, "video.tilemap", { { "layer", "background" }, { "format", "rgba" } } );
  REQUIRE( map.at( "result" ).at( "width" ) == 2048 );
  REQUIRE( map.at( "result" ).at( "height" ) == 512 );
  REQUIRE( call( dispatcher, "video.tilemap", { { "layer", "text" } } ).at( "result" ).at( "width" ) == 512 );

  REQUIRE( call( dispatcher, "video.tiles", { { "layer", "sprites" } } ).at( "error" ).at( "code" ) == "bad_request" );
  REQUIRE( call( dispatcher, "video.tiles", { { "layer", "text" }, { "count", 5000 } } ).at( "error" ).at( "code" ) ==
           "bad_request" );
}
