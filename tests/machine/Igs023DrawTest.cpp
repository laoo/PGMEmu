#include <catch2/catch_test_macros.hpp>

#include "machine/Igs023.hpp"
#include "machine/Sdram.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using pgm::machine::Igs023;
using pgm::machine::Time;
using pgm::machine::UNITS_PER_FRAME;

namespace
{

constexpr std::uint32_t VRAM = 0x900000;
constexpr std::uint32_t PALETTE = 0xa00000;
constexpr std::uint32_t TEXT_MAP = VRAM + 0x4000;

/// The chip with a tile ROM of its own in the BIOS's tile region.
class Chip
{
public:
  Chip() : mTiles( 0x10000, 0 )
  {
    mSdram.biosTiles = mTiles;
  }

  void tileBytes( std::size_t at, std::vector<std::uint8_t> const& bytes )
  {
    std::ranges::copy( bytes, mTiles.begin() + static_cast<std::ptrdiff_t>( at ) );
  }

  void word( std::uint32_t address, std::uint16_t value )
  {
    mVideo.write( 0, address, value, true, true );
  }

  /// Colour of the palette word `index`: its red channel is the index, so a
  /// pixel's red byte tells which word was used.
  void markPalette( std::uint32_t index )
  {
    word( PALETTE + ( index * 2 ), static_cast<std::uint16_t>( ( index & 0x1fU ) << 10U ) );
  }

  /// The red channel's 5 bits at `x`, `y` of the first complete picture.
  std::uint32_t redAt( int x, int y )
  {
    mVideo.advanceTo( UNITS_PER_FRAME + 100 );
    auto const picture = mVideo.frame();
    return picture[( ( static_cast<std::size_t>( y ) * Igs023::WIDTH ) + static_cast<std::size_t>( x ) ) * 4] >> 3U;
  }

private:
  std::vector<std::uint8_t> mTiles;
  pgm::machine::Sdram mSdram{};
  std::array<std::uint8_t, 0x20000> mWorkRam{};
  Igs023 mVideo{ mSdram, pgm::machine::TileMapping{}, mWorkRam };
};

} // namespace

TEST_CASE( "the text layer draws 4-bit tiles through its palette, 0xF transparent", "[machine][igs023]" )
{
  Chip chip;
  // Tile 1's first row: pixels 1, 2, 3 ... 7 and a transparent one, a nibble
  // each, lowest first.
  chip.tileBytes( 32, { 0x21, 0x43, 0x65, 0xf7 } );
  chip.word( TEXT_MAP, 1 );
  chip.word( TEXT_MAP + 2, 2U << 1U ); // palette 2
  for ( std::uint32_t pixel = 0; pixel < 16; ++pixel )
  {
    chip.markPalette( 0x800 + ( 2 * 16 ) + pixel );
  }
  chip.markPalette( 0x400 ); // the background's tile 0, colour 0, under the hole

  // Red 5 bits of word 0x820 + n are its low 5 bits: 0x00 + n.
  REQUIRE( chip.redAt( 0, 0 ) == 1 );
  REQUIRE( chip.redAt( 6, 0 ) == 7 );
  REQUIRE( chip.redAt( 7, 0 ) == ( 0x400 & 0x1fU ) );
}

TEST_CASE( "a text tile flipped horizontally reads its row backwards", "[machine][igs023]" )
{
  Chip chip;
  chip.tileBytes( 32, { 0x21, 0x43, 0x65, 0x87 } );
  chip.word( TEXT_MAP, 1 );
  chip.word( TEXT_MAP + 2, 0x40 ); // flip X, palette 0
  for ( std::uint32_t pixel = 0; pixel < 16; ++pixel )
  {
    chip.markPalette( 0x800 + pixel );
  }

  REQUIRE( chip.redAt( 0, 0 ) == 8 );
  REQUIRE( chip.redAt( 7, 0 ) == 1 );
}

TEST_CASE( "the background draws 5-bit tiles as a stream, scrolled per line", "[machine][igs023]" )
{
  Chip chip;
  // Background tile 2's rows are 20 bytes each from (2 * 32) * 20; pixel 0 is
  // a row's lowest 5 bits, pixel 1 the next 5. Rows 0 and 1 are alike.
  for ( std::size_t row = 0; row < 2; ++row )
  {
    chip.tileBytes( ( std::size_t{ 2 } * 32 * 20 ) + ( row * 20 ),
                    { static_cast<std::uint8_t>( 3U | ( 4U << 5U ) ), static_cast<std::uint8_t>( 4U >> 3U ) } );
  }
  chip.word( VRAM, 2 );
  chip.word( VRAM + 2, 1U << 1U ); // palette 1
  for ( std::uint32_t pixel = 0; pixel < 8; ++pixel )
  {
    chip.markPalette( 0x400 + 32 + pixel );
  }
  // Line 1 is scrolled one pixel to the left.
  chip.word( VRAM + 0x7000 + 2, 1 );
  for ( std::uint32_t column = 0; column < 64; ++column )
  {
    chip.word( TEXT_MAP + ( column * 4 ), 0 ); // text tile 0: transparent
  }
  chip.tileBytes( 0, std::vector<std::uint8_t>( 32, 0xff ) );

  REQUIRE( chip.redAt( 0, 0 ) == ( ( 0x420 + 3 ) & 0x1fU ) );
  REQUIRE( chip.redAt( 1, 0 ) == ( ( 0x420 + 4 ) & 0x1fU ) );
  REQUIRE( chip.redAt( 0, 1 ) == ( ( 0x420 + 4 ) & 0x1fU ) );
}

TEST_CASE( "the flags register turns the text layer and the background off", "[machine][igs023]" )
{
  constexpr std::uint32_t flags = 0xb0e000;
  Chip chip;
  chip.tileBytes( 32, { 0x11, 0x11, 0x11, 0x11 } );    // text tile 1: colour 1
  chip.tileBytes( std::size_t{ 2 } * 32 * 20, { 3 } ); // background tile 2: colour 3 first
  chip.word( TEXT_MAP, 1 );
  chip.word( VRAM, 2 );
  chip.markPalette( 0x801 );
  chip.markPalette( 0x403 );
  chip.markPalette( 0x3ff );

  SECTION( "both on: the text" )
  {
    REQUIRE( chip.redAt( 0, 0 ) == 1 );
  }

  SECTION( "the text off: the background" )
  {
    chip.word( flags, 1U << 11U );
    REQUIRE( chip.redAt( 0, 0 ) == 3 );
  }

  SECTION( "both off: the backdrop, a low-priority sprite pixel no sprite drew" )
  {
    chip.word( flags, 3U << 11U );
    REQUIRE( chip.redAt( 0, 0 ) == 0x1f );
  }
}
