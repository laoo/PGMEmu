#include <catch2/catch_test_macros.hpp>

#include "machine/Sdram.hpp"
#include "machine/SpriteEngine.hpp"

#include <array>
#include <memory>
#include <vector>

using pgm::machine::Sdram;
using pgm::machine::SpriteFrame;
using pgm::machine::SpriteList;

namespace
{

/// Mask (B) and colour (A) ROMs written word by word, low byte first.
class Roms
{
public:
  Roms() : mMask( 0x1000, 0 ), mColour( 0x1000, 0 ) {}

  void mask( std::size_t word, std::uint16_t value )
  {
    put( mMask, word, value );
  }

  void colour( std::size_t word, std::uint16_t value )
  {
    put( mColour, word, value );
  }

  [[nodiscard]] Sdram sdram() const
  {
    Sdram sdram{};
    sdram.cartBRom = mMask;
    sdram.cartARom = mColour;
    return sdram;
  }

private:
  static void put( std::vector<std::uint8_t>& rom, std::size_t word, std::uint16_t value )
  {
    rom.at( word * 2 ) = static_cast<std::uint8_t>( value );
    rom.at( ( word * 2 ) + 1 ) = static_cast<std::uint8_t>( value >> 8U );
  }

  std::vector<std::uint8_t> mMask;
  std::vector<std::uint8_t> mColour;
};

/// A sprite one 16-pixel word wide and `height` lines high, whose mask starts
/// at B-ROM word 0x10 and whose colours start at A-ROM pixel 0.
std::array<std::uint16_t, 5>
sprite( std::uint16_t x, std::uint16_t y, std::uint16_t height, std::uint16_t scaleX = 16, bool flipX = false )
{
  return { static_cast<std::uint16_t>( ( scaleX << 11U ) | x ),
           static_cast<std::uint16_t>( ( 16U << 11U ) | y ),
           static_cast<std::uint16_t>( ( flipX ? 0x2000U : 0U ) | ( 3U << 8U ) ),
           0x10,
           static_cast<std::uint16_t>( ( 1U << 9U ) | height ) };
}

/// Colours 1, 2, 3... in A-ROM, three to a word.
void countingColours( Roms& roms )
{
  for ( std::uint16_t word = 0; word < 16; ++word )
  {
    auto const first = static_cast<std::uint16_t>( ( word * 3 ) + 1 );
    roms.colour( word, static_cast<std::uint16_t>( first | ( ( first + 1 ) << 5U ) | ( ( first + 2 ) << 10U ) ) );
  }
}

std::uint16_t colourAt( SpriteFrame const& frame, std::size_t line, std::size_t x )
{
  return static_cast<std::uint16_t>( frame.at( line ).at( x ) & 0x1fU );
}

} // namespace

TEST_CASE( "a sprite's mask decides its pixels, and only opaque ones take colours", "[machine][sprites]" )
{
  Roms roms;
  countingColours( roms );
  roms.mask( 0x10, 0x0000 ); // the A-ROM start, low and high word
  roms.mask( 0x11, 0x0000 );
  roms.mask( 0x12, 0xfff2 ); // pixels 0, 2 and 3 opaque

  SpriteList list;
  list.entries[0] = sprite( 100, 50, 1 );
  list.count = 1;
  auto frame = std::make_unique<SpriteFrame>();
  pgm::machine::drawSprites( list, roms.sdram(), *frame );

  REQUIRE( colourAt( *frame, 50, 100 ) == 1 );
  REQUIRE( frame->at( 50 ).at( 101 ) == pgm::machine::SPRITE_ERASED );
  REQUIRE( colourAt( *frame, 50, 102 ) == 2 );
  REQUIRE( colourAt( *frame, 50, 103 ) == 3 );
  // Drawn pixels carry the sprite's palette and the drawn bit.
  REQUIRE( ( frame->at( 50 ).at( 100 ) & 0xbe0U ) == ( 0x800U | ( 3U << 5U ) ) );
  REQUIRE( frame->at( 49 ).at( 100 ) == pgm::machine::SPRITE_ERASED );
  REQUIRE( frame->at( 51 ).at( 100 ) == pgm::machine::SPRITE_ERASED );
}

TEST_CASE( "a flipped sprite is drawn from its right edge", "[machine][sprites]" )
{
  Roms roms;
  countingColours( roms );
  roms.mask( 0x12, 0xfffc ); // pixels 0 and 1 opaque

  SpriteList list;
  list.entries[0] = sprite( 100, 10, 1, 16, true );
  list.count = 1;
  auto frame = std::make_unique<SpriteFrame>();
  pgm::machine::drawSprites( list, roms.sdram(), *frame );

  REQUIRE( colourAt( *frame, 10, 115 ) == 1 );
  REQUIRE( colourAt( *frame, 10, 114 ) == 2 );
  REQUIRE( frame->at( 10 ).at( 100 ) == pgm::machine::SPRITE_ERASED );
}

TEST_CASE( "zoom 0 drops every other pixel and 31 doubles most of them", "[machine][sprites]" )
{
  Roms roms;
  countingColours( roms );
  roms.mask( 0x12, 0x0000 ); // all 16 opaque

  SpriteList list;
  list.entries[0] = sprite( 0, 0, 1, 0 );    // pattern 0xAAAAAAAA: odd pixels dropped
  list.entries[1] = sprite( 200, 1, 1, 24 ); // pattern 0x11111111: every 4th pixel twice
  list.count = 2;
  auto frame = std::make_unique<SpriteFrame>();
  pgm::machine::drawSprites( list, roms.sdram(), *frame );

  REQUIRE( colourAt( *frame, 0, 0 ) == 1 );
  REQUIRE( colourAt( *frame, 0, 1 ) == 3 );
  REQUIRE( colourAt( *frame, 0, 7 ) == 15 );
  REQUIRE( frame->at( 0 ).at( 8 ) == pgm::machine::SPRITE_ERASED );

  REQUIRE( colourAt( *frame, 1, 200 ) == 1 );
  REQUIRE( colourAt( *frame, 1, 201 ) == 1 );
  REQUIRE( colourAt( *frame, 1, 202 ) == 2 );
  REQUIRE( colourAt( *frame, 1, 205 ) == 5 );
  REQUIRE( colourAt( *frame, 1, 206 ) == 5 );
}

TEST_CASE( "a later sprite in the list is drawn over an earlier one", "[machine][sprites]" )
{
  Roms roms;
  countingColours( roms );
  roms.mask( 0x12, 0x0000 );

  SpriteList list;
  list.entries[0] = sprite( 10, 5, 1 );
  list.entries[1] = sprite( 12, 5, 1 );
  list.count = 2;
  auto frame = std::make_unique<SpriteFrame>();
  pgm::machine::drawSprites( list, roms.sdram(), *frame );

  REQUIRE( colourAt( *frame, 5, 12 ) == 1 );
  REQUIRE( colourAt( *frame, 5, 11 ) == 2 );
}

TEST_CASE( "the DMA copies entries up to the first of size zero", "[machine][sprites]" )
{
  std::vector<std::uint8_t> workRam( 0x20000, 0 );
  for ( std::size_t index = 0; index < 3; ++index )
  {
    workRam.at( ( ( index * 5 ) + 4 ) * 2 ) = 0x02; // a non-zero size word
  }

  SpriteList const list = pgm::machine::copySpriteList( workRam );

  REQUIRE( list.count == 3 );
  REQUIRE( pgm::machine::wordsCopied( list ) == 20 );
}
