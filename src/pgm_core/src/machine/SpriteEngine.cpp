#include "SpriteEngine.hpp"

#include <bit>
#include <utility>
#include <vector>

namespace pgm::machine
{

namespace
{

constexpr int SCREEN_WIDTH = 448;
constexpr int SCREEN_LINES = 224;

// igs023_sprite.sv: which pixels and lines a zoom setting repeats (in enlarge
// mode) or drops (in shrink mode), one bit per source pixel or line, cyclic.
constexpr std::array<std::uint32_t, 32> SCALE_PATTERN{
  0xAAAAAAAA, 0xA8AAAAAA, 0xA8AAA8AA, 0xA8A8A8AA, 0xA8A8A8A8, 0x88A8A8A8, 0x88A888A8, 0x888888A8,
  0x88888888, 0x80888888, 0x80888088, 0x80808088, 0x80808080, 0x80008080, 0x80008000, 0x00008000,
  0x00000000, 0x00010000, 0x00010001, 0x01010001, 0x01010101, 0x01110101, 0x01110111, 0x11110111,
  0x11111111, 0x11511111, 0x11511151, 0x51511151, 0x51515151, 0x51555151, 0x51555155, 0x55555155
};

/// A position in the colour ROM: a 16-bit word, and which of the three 5-bit
/// pixels it holds (arom_offset_t).
struct ColourOffset
{
  std::uint32_t words{};
  std::uint32_t sub{};

  void advance( std::uint32_t pixels, bool backwards )
  {
    std::uint32_t const words3 = pixels / 3;
    std::uint32_t const sub3 = pixels % 3;
    if ( !backwards )
    {
      std::uint32_t const sum = sub + sub3;
      words = ( words + words3 + ( sum > 2 ? 1 : 0 ) ) & 0x1ffffffU;
      sub = sum > 2 ? sum - 3 : sum;
    }
    else if ( sub >= sub3 )
    {
      words = ( words - words3 ) & 0x1ffffffU;
      sub -= sub3;
    }
    else
    {
      words = ( words - words3 - 1 ) & 0x1ffffffU;
      sub = sub + 3 - sub3;
    }
  }
};

/// What changes as a sprite is drawn (volatile_sprite_state_t, less the cache).
struct Progress
{
  std::uint16_t maskOffset{};
  ColourOffset colour;
  std::uint16_t screenLine{}; // 10 bits
  std::uint16_t sourceLine{}; // 9 bits
  bool active{};
  bool repeated{};
};

/// What a sprite's five words say.
struct Sprite
{
  std::uint32_t x{}; // 11 bits
  std::uint32_t y{}; // 10 bits
  std::uint32_t scaleX{};
  std::uint32_t scaleY{};
  bool flipX{};
  bool flipY{};
  std::uint16_t palette{};
  bool lowPriority{};
  std::uint32_t maskAddress{}; // 23-bit word address in the B ROM
  std::uint32_t width{};       // in 16-pixel words
  std::uint32_t height{};
  std::uint32_t maskBase{};
  std::uint32_t scaledWidth{};

  explicit Sprite( std::array<std::uint16_t, SpriteList::WORDS> const& words )
  {
    SpriteInfo const info = decodeSprite( words );
    x = info.x;
    scaleX = info.scaleX;
    y = info.y;
    scaleY = info.scaleY;
    maskAddress = info.maskAddress;
    lowPriority = info.lowPriority;
    palette = info.palette;
    flipX = info.flipX;
    flipY = info.flipY;
    height = info.height;
    width = info.width;
    // Read backwards, a sprite starts where the next one's header is: past its
    // two header words and all its masks.
    maskBase = flipY ? ( maskAddress + 3 + ( width * height ) ) & 0x7fffffU : maskAddress;
    std::uint32_t const width32 = scaleX + 16;
    scaledWidth = ( ( width32 * ( width >> 1U ) ) + ( ( width & 1U ) != 0 ? width32 >> 1U : 0 ) ) & 0xfffU;
  }

  [[nodiscard]] bool zoomX() const
  {
    return ( scaleX & 0x10U ) != 0;
  }

  [[nodiscard]] bool zoomY() const
  {
    return ( scaleY & 0x10U ) != 0;
  }

  [[nodiscard]] bool lineBit( std::uint16_t sourceLine ) const
  {
    return ( ( SCALE_PATTERN.at( scaleY ) >> ( sourceLine & 31U ) ) & 1U ) != 0;
  }
};

class Drawer
{
public:
  Drawer( Sprite const& sprite, Sdram const& sdram ) : mSprite{ sprite }, mSdram{ sdram } {}

  [[nodiscard]] std::uint16_t mask( std::uint16_t offset ) const
  {
    std::uint32_t const address = ( mSprite.flipY ? mSprite.maskBase - offset : mSprite.maskBase + offset ) & 0x7fffffU;
    return mSdram.word( Sdram::CART_B_ROM_AT + ( address * 2 ) );
  }

  [[nodiscard]] std::uint16_t colour( ColourOffset const& at ) const
  {
    std::uint16_t const word = mSdram.word( Sdram::CART_A_ROM_AT + ( at.words * 2 ) );
    return static_cast<std::uint16_t>( ( word >> ( 5 * at.sub ) ) & 0x1fU );
  }

  /// Opaque pixels of a mask word: those whose bit is clear.
  static std::uint32_t opaque( std::uint16_t mask )
  {
    return static_cast<std::uint32_t>( 16 - std::popcount( mask ) );
  }

  /// PRESCAN: where the sprite stands at the first line on screen.
  [[nodiscard]] Progress prescan() const
  {
    Progress state;
    std::uint32_t const start = ( static_cast<std::uint32_t>( mask( 1 ) ) << 16U ) | mask( 0 );
    state.colour = ColourOffset{ .words = ( start >> 2U ) & 0x1ffffffU, .sub = start & 3U };
    state.maskOffset = 2;
    state.screenLine = static_cast<std::uint16_t>( mSprite.y );
    state.active = true;
    Progress saved = state;

    std::uint32_t column = 0;
    auto const lastLine = static_cast<std::uint16_t>( ( mSprite.height - 1 ) & 0x1ffU );
    while ( true )
    {
      if ( mSprite.height == 0 )
      {
        state.active = false;
        return state;
      }
      if ( ( state.screenLine & 0x200U ) == 0 )
      {
        return state;
      }
      if ( column == mSprite.width )
      {
        Progress const before = state;
        if ( mSprite.zoomY() )
        {
          state.screenLine = ( state.screenLine + 1 ) & 0x3ffU;
          state.repeated = false;
          if ( before.repeated || !mSprite.lineBit( before.sourceLine ) )
          {
            state.sourceLine = ( state.sourceLine + 1 ) & 0x1ffU;
            saved = before;
          }
          else
          {
            state.repeated = true;
            state.colour = saved.colour;
            state.maskOffset = saved.maskOffset;
          }
        }
        else
        {
          state.sourceLine = ( state.sourceLine + 1 ) & 0x1ffU;
          if ( !mSprite.lineBit( before.sourceLine ) )
          {
            state.screenLine = ( state.screenLine + 1 ) & 0x3ffU;
          }
          saved = before;
        }
        column = 0;
        if ( before.sourceLine == lastLine )
        {
          state.active = false;
          return state;
        }
      }
      else
      {
        state.colour.advance( opaque( mask( state.maskOffset ) ), mSprite.flipY );
        ++state.maskOffset;
        ++column;
      }
    }
  }

  /// DRAW_ROW to DRAW_ROW_END: one line of the sprite into `line`, and the
  /// state moved on to the next. `saved` is the state the line started from.
  void drawRow( Progress& state, SpriteLine& line ) const
  {
    Progress const saved = state;
    std::uint32_t xBits = SCALE_PATTERN.at( mSprite.scaleX );
    std::uint32_t position = 0;
    bool const mirrored = mSprite.flipX != mSprite.flipY;
    auto const tag = static_cast<std::uint16_t>( 0x800U | ( mSprite.lowPriority ? 0x400U : 0U ) |
                                                 ( static_cast<std::uint32_t>( mSprite.palette ) << 5U ) );

    for ( std::uint32_t column = 0; column < mSprite.width; ++column )
    {
      std::uint16_t bits = mask( state.maskOffset );
      if ( mSprite.flipY )
      {
        bits = reverse( bits );
      }
      for ( int pixel = 0; pixel < 16; ++pixel )
      {
        bool const transparent = ( bits & 1U ) != 0;
        bool const scaleBit = ( xBits & 1U ) != 0;
        std::uint32_t copies = 1;
        if ( mSprite.zoomX() )
        {
          copies = scaleBit ? 2 : 1;
        }
        else
        {
          copies = scaleBit ? 0 : 1;
        }
        if ( !transparent )
        {
          auto const value = static_cast<std::uint16_t>( tag | colour( state.colour ) );
          for ( std::uint32_t copy = 0; copy < copies; ++copy )
          {
            std::uint32_t const at =
                mirrored ? mSprite.x + mSprite.scaledWidth - 1 - ( position + copy ) : mSprite.x + position + copy;
            std::uint32_t const screenX = at & 0x7ffU;
            if ( screenX < SCREEN_WIDTH )
            {
              line.at( screenX ) = value;
            }
          }
          state.colour.advance( 1, mSprite.flipY );
        }
        position += copies;
        bits = static_cast<std::uint16_t>( bits >> 1U );
        xBits = std::rotr( xBits, 1 );
      }
      ++state.maskOffset;
    }

    std::uint16_t const before = state.sourceLine;
    auto const lastLine = static_cast<std::uint16_t>( ( mSprite.height - 1 ) & 0x1ffU );
    if ( mSprite.zoomY() )
    {
      bool const wasRepeated = state.repeated;
      state.screenLine = ( state.screenLine + 1 ) & 0x3ffU;
      state.repeated = false;
      if ( wasRepeated || !mSprite.lineBit( before ) )
      {
        state.sourceLine = ( state.sourceLine + 1 ) & 0x1ffU;
      }
      else
      {
        state.repeated = true;
        state.colour = saved.colour;
        state.maskOffset = saved.maskOffset;
      }
    }
    else
    {
      state.screenLine = ( state.screenLine + 1 ) & 0x3ffU;
      state.sourceLine = ( state.sourceLine + 1 ) & 0x1ffU;
    }
    if ( before == lastLine )
    {
      state.active = false;
    }

    // In shrink mode a line whose bit is set is followed by one that is skipped.
    if ( !mSprite.zoomY() && mSprite.lineBit( before ) )
    {
      for ( std::uint32_t column = 0; column < mSprite.width; ++column )
      {
        state.colour.advance( opaque( mask( state.maskOffset ) ), mSprite.flipY );
        ++state.maskOffset;
      }
      if ( state.sourceLine == lastLine )
      {
        state.active = false;
      }
      state.sourceLine = ( state.sourceLine + 1 ) & 0x1ffU;
    }
  }

private:
  static std::uint16_t reverse( std::uint16_t value )
  {
    std::uint16_t result = 0;
    for ( unsigned bit = 0; bit < 16; ++bit )
    {
      result = static_cast<std::uint16_t>( ( static_cast<unsigned>( result ) << 1U ) | ( ( value >> bit ) & 1U ) );
    }
    return result;
  }

  Sprite const& mSprite;
  Sdram const& mSdram;
};

} // namespace

SpriteInfo decodeSprite( std::array<std::uint16_t, SpriteList::WORDS> const& words )
{
  return SpriteInfo{ .x = words[0] & 0x7ffU,
                     .y = words[1] & 0x3ffU,
                     .scaleX = ( words[0] >> 11U ) & 0x1fU,
                     .scaleY = ( words[1] >> 11U ) & 0x1fU,
                     .flipX = ( words[2] & 0x2000U ) != 0,
                     .flipY = ( words[2] & 0x4000U ) != 0,
                     .lowPriority = ( words[2] & 0x80U ) != 0,
                     .palette = static_cast<std::uint16_t>( ( words[2] >> 8U ) & 0x1fU ),
                     .maskAddress = ( static_cast<std::uint32_t>( words[2] & 0x7fU ) << 16U ) | words[3],
                     .width = ( words[4] >> 9U ) & 0x3fU,
                     .height = words[4] & 0x1ffU };
}

SpriteList copySpriteList( std::span<std::uint8_t const> workRam )
{
  SpriteList list;
  for ( std::size_t index = 0; index < SpriteList::MAX_SPRITES; ++index )
  {
    auto& entry = list.entries.at( index );
    for ( std::size_t word = 0; word < SpriteList::WORDS; ++word )
    {
      std::size_t const at = ( ( index * SpriteList::WORDS ) + word ) * 2;
      entry.at( word ) = static_cast<std::uint16_t>( ( workRam[at] << 8U ) | workRam[at + 1] );
    }
    if ( ( entry[4] & 0x7fffU ) == 0 || index == SpriteList::MAX_SPRITES - 1 )
    {
      list.count = index;
      return list;
    }
  }
  return list;
}

std::size_t wordsCopied( SpriteList const& list )
{
  return ( list.count + 1 ) * SpriteList::WORDS;
}

void drawSprites( SpriteList const& list, Sdram const& sdram, SpriteFrame& frame )
{
  for ( SpriteLine& line : frame )
  {
    line.fill( SPRITE_ERASED );
  }

  std::vector<Sprite> sprites;
  std::vector<Progress> progress;
  sprites.reserve( list.count );
  progress.reserve( list.count );
  for ( std::size_t index = 0; index < list.count; ++index )
  {
    sprites.emplace_back( list.entries.at( index ) );
  }
  for ( Sprite const& sprite : sprites )
  {
    progress.push_back( Drawer{ sprite, sdram }.prescan() );
  }

  // Line by line, every sprite in list order: a later sprite draws over an
  // earlier one, as the line buffer is written in that order.
  for ( int line = 0; line < SCREEN_LINES; ++line )
  {
    for ( std::size_t index = 0; index < sprites.size(); ++index )
    {
      Progress& state = progress[index];
      if ( state.active && std::cmp_equal( state.screenLine, line ) )
      {
        Drawer{ sprites[index], sdram }.drawRow( state, frame.at( static_cast<std::size_t>( line ) ) );
      }
    }
  }
}

} // namespace pgm::machine
