#include "Igs023.hpp"

#include <algorithm>

#include <array>

namespace pgm::machine
{

namespace
{

// ctrl[14], the flags register at 0xB0E000.
constexpr std::uint16_t IRQ4_ENABLE = 1U << 2U;
constexpr std::uint16_t IRQ6_ENABLE = 1U << 3U;
constexpr std::size_t FLAGS = 14;
constexpr std::size_t LINE_COUNTER = 7;
constexpr std::size_t ZOOM_TABLE = 1;

constexpr int IRQ4_PERIOD_LINES = 62;
constexpr std::size_t SCROLL_BG_Y = 2;
constexpr std::size_t SCROLL_BG_X = 3;
constexpr std::size_t SCROLL_FG_Y = 5;
constexpr std::size_t SCROLL_FG_X = 6;
constexpr std::uint16_t SPRITE_DMA_ENABLE = 1U << 0U;
constexpr std::uint16_t CPU_BUS_MASTER = 1U << 10U;
constexpr std::uint16_t TEXT_DISABLE = 1U << 11U;
constexpr std::uint16_t BACKGROUND_DISABLE = 1U << 12U;
// Only high-priority sprites; low-priority ones are drawn regardless.
constexpr std::uint16_t SPRITES_DISABLE = 1U << 13U;

// The text layer's fetch holds VRAM from the master tick after the dot counter
// reaches 638, for 464 pulses of ce_33m: 464 * 908 / 615 master ticks.
constexpr Time FETCH_START = ( Time{ 638 } * UNITS_PER_DOT ) + UNITS_PER_MASTER_TICK;
constexpr Time FETCH_LENGTH = ( Time{ 464 } * 908 * UNITS_PER_MASTER_TICK ) / 615;
constexpr std::uint16_t SPRITE_DMA_LINE = 221;

// The VRAM arbiter counts master ticks; the pixel enable fires on every fifth.
constexpr std::int64_t TICKS_PER_DOT = UNITS_PER_DOT / UNITS_PER_MASTER_TICK;
constexpr std::int64_t TICKS_PER_LINE = DOTS_PER_LINE * TICKS_PER_DOT;
constexpr std::int64_t FETCH_START_TICK = FETCH_START / UNITS_PER_MASTER_TICK;
constexpr std::int64_t FETCH_TICKS = FETCH_LENGTH / UNITS_PER_MASTER_TICK;
// The background's head start at alignment 0, in dots (igs023_bg.sv, from the
// board: the lock ends 11.8 us after hsync, 100 ns sooner per pixel of scroll).
constexpr std::int64_t HEADSTART_DOTS = 47;
constexpr std::int64_t MICROCYCLE_DOTS = 8;
// From the tick the arbiter starts an access to the one it acknowledges on:
// both bytes, a setup and a hold tick each.
constexpr std::int64_t ACCESS_TICKS = 4;
// The text layer's fetch for the next line is made on lines 39 to 262; from
// the first of them to the end of the frame the arbiter keeps its schedule.
constexpr int FIRST_FETCH_LINE = VBLANK_LINES - 1;
constexpr int LAST_FETCH_LINE = LINES_PER_FRAME - 2;

bool fetchedOn( std::int64_t line )
{
  auto const vcnt = static_cast<int>( line % LINES_PER_FRAME );
  return vcnt >= FIRST_FETCH_LINE && vcnt <= LAST_FETCH_LINE;
}

// The dot a master tick is in, counted from power-up: the counter advances on
// the tick after each pixel enable (dotsAt below).
std::int64_t dotOfTick( std::int64_t tick )
{
  return tick < 1 ? 0 : ( tick - 1 ) / TICKS_PER_DOT;
}

// The dots of a line at which the raster does something: the line starts, its
// hsync rises, and the layers start fetching the next line.
constexpr std::array<int, 3> EVENT_DOTS{ 0, HSYNC_START_DOT, 638 };

// Palette words of each layer (igs023.sv's mixer).
constexpr std::uint32_t SPRITE_PALETTE = 0x000;
constexpr std::uint32_t BACKGROUND_PALETTE = 0x400;
constexpr std::uint32_t BACKDROP = 0x3ff;
constexpr std::uint32_t TEXT_PALETTE = 0x800;

// A layer pixel, as the mixer is handed it: the palette word, its transparent
// pen included, or NONE where the debugger has hidden the layer. A tile's
// transparent pen is its last: 0xf of the text's 4 bits, 0x1f of the
// background's 5, which NONE also has.
constexpr std::uint16_t NONE = 0xffff;

bool textOpaque( std::uint16_t pixel )
{
  return ( pixel & 0xfU ) != 0xfU;
}

bool backgroundOpaque( std::uint16_t pixel )
{
  return ( pixel & 0x1fU ) != 0x1fU;
}

// Which dot of the raster is current after `ticks` master ticks. The RTL's
// pixel enable first fires on the fifth clock after power-up and on every
// fifth after that, and the dot counter advances on the clock that follows.
std::int64_t dotsAt( Time now )
{
  std::int64_t const ticks = masterTicks( now );
  return ticks < 1 ? 0 : ( ticks - 1 ) / 5;
}

// Where in VRAM a byte lands: the RTL folds the background map into 4 KB and
// 0x6000-0x6FFF onto the text map (vram_phys in igs023.sv).
std::size_t physical( std::size_t address )
{
  if ( ( address & 0x4000U ) == 0 )
  {
    return address & 0x0fffU;
  }
  if ( ( address & 0x3000U ) == 0x2000U )
  {
    return 0x4000U | ( address & 0x0fffU );
  }
  return address;
}

} // namespace

Igs023::Igs023( Sdram const& sdram, TileMapping tiles, std::span<std::uint8_t const> workRam )
    : mSdram{ sdram }, mTiles{ tiles }, mWorkRam{ workRam }, mSprites{ std::make_unique<SpriteFrame>() },
      mNextSprites{ std::make_unique<SpriteFrame>() }, mBuilding( static_cast<std::size_t>( WIDTH * HEIGHT * 4 ), 0 ),
      mFrame( static_cast<std::size_t>( WIDTH * HEIGHT * 4 ), 0 )
{
  // The RTL's buffers hold zeros until each line is first shown and erased;
  // they start erased here (docs/hardware/differences.md).
  for ( SpriteFrame* frame : { mSprites.get(), mNextSprites.get() } )
  {
    for ( SpriteLine& line : *frame )
    {
      line.fill( SPRITE_ERASED );
    }
  }
}

void Igs023::advanceTo( Time now )
{
  // Event 3k + n is the n-th event of line k counted from power-up. Each is
  // seen by the logic a master tick after the dot counter reaches its dot,
  // which a dot's granularity does not resolve.
  std::int64_t const dots = dotsAt( now );
  std::int64_t const lines = dots / DOTS_PER_LINE;
  int const dot = static_cast<int>( dots % DOTS_PER_LINE );
  std::int64_t passed = 0;
  for ( int const eventDot : EVENT_DOTS )
  {
    passed += dot >= eventDot ? 1 : 0;
  }
  std::int64_t const events = ( lines * 3 ) + passed;
  while ( mEventsDone < events )
  {
    std::int64_t const event = mEventsDone++;
    std::int64_t const line = event / 3;
    int const vcnt = static_cast<int>( line % LINES_PER_FRAME );
    switch ( event % 3 )
    {
    case 0:
      onLineStart( vcnt );
      break;
    case 1:
      onHsync( ( ( line * DOTS_PER_LINE ) + HSYNC_START_DOT ) * UNITS_PER_DOT );
      break;
    default:
      onFetch( line );
      break;
    }
  }
}

void Igs023::onLineStart( int line )
{
  if ( line == 0 )
  {
    // Vertical blank begins: the frame just drawn is complete, and the sprites
    // the last DMA copied are those of the next.
    mFrame.swap( mBuilding );
    ++mFramesCompleted;
    if ( mNextSpritesReady )
    {
      mSprites.swap( mNextSprites );
      mNextSpritesReady = false;
    }
    if ( ( controlFlags() & IRQ6_ENABLE ) != 0 )
    {
      mIrq6 = true;
    }
  }
  if ( line == VBLANK_LINES )
  {
    mControl[LINE_COUNTER] = 0;
  }
}

void Igs023::onHsync( Time at )
{
  if ( mControl[LINE_COUNTER] == SPRITE_DMA_LINE && ( controlFlags() & SPRITE_DMA_ENABLE ) != 0 )
  {
    // The DMA takes the bus from the 68000 and reads the list four master
    // ticks a word (igs023_sprite.sv), then the sprites are drawn from it.
    mSpriteList = copySpriteList( mWorkRam );
    drawSprites( mSpriteList, mSdram, *mNextSprites );
    mNextSpritesReady = true;
    auto const words = static_cast<Time>( wordsCopied( mSpriteList ) );
    mBusHeldUntil = at + ( ( ( words * 4 ) + 4 ) * UNITS_PER_MASTER_TICK );
  }

  ++mControl[LINE_COUNTER];
  if ( mIrq4Count == IRQ4_PERIOD_LINES - 1 )
  {
    mIrq4Count = 0;
    if ( ( controlFlags() & IRQ4_ENABLE ) != 0 )
    {
      mIrq4 = true;
    }
  }
  else
  {
    ++mIrq4Count;
  }
}

void Igs023::onFetch( std::int64_t line )
{
  // The fetch at the end of line `line` is for the next one.
  int const next = static_cast<int>( line % LINES_PER_FRAME ) + 1 - VBLANK_LINES;
  if ( next >= 0 && next < HEIGHT )
  {
    mFetchAlignments.at( static_cast<std::size_t>( line ) % mFetchAlignments.size() ) =
        FetchAlignment{ .line = line, .alignment = backgroundScrollX( next ) & 31U };
    drawLine( next );
  }
}

std::uint32_t Igs023::backgroundScrollX( int line ) const
{
  // The X register plus the line's scroll word at 0x7000.
  std::uint32_t const scrollAt = 0x7000U + ( static_cast<std::uint32_t>( line ) << 1U );
  return ( mControl[SCROLL_BG_X] + vramWord( scrollAt ) ) & 0x7ffU;
}

std::uint32_t Igs023::fetchAlignment( std::int64_t line ) const
{
  FetchAlignment const& made = mFetchAlignments.at( static_cast<std::size_t>( line ) % mFetchAlignments.size() );
  if ( made.line == line )
  {
    return made.alignment;
  }
  // A fetch not made yet will see the registers and VRAM as they are now.
  int const next = static_cast<int>( line % LINES_PER_FRAME ) + 1 - VBLANK_LINES;
  return backgroundScrollX( next ) & 31U;
}

Igs023::FetchLock Igs023::fetchLock( std::int64_t line ) const
{
  FetchLock lock;
  lock.textStart = ( line * TICKS_PER_LINE ) + FETCH_START_TICK;
  lock.textEnd = lock.textStart + FETCH_TICKS;
  // The head start is loaded on the tick after the text layer lets go, and
  // counted down by the pixel enables after that; the lock ends on the tick
  // after the last.
  std::int64_t const firstEnable = lock.textEnd - ( lock.textEnd % TICKS_PER_DOT ) + TICKS_PER_DOT;
  std::int64_t const dots = HEADSTART_DOTS - static_cast<std::int64_t>( fetchAlignment( line ) );
  lock.end = firstEnable + ( ( dots - 1 ) * TICKS_PER_DOT ) + 1;
  return lock;
}

std::uint8_t Igs023::vramAt( std::size_t address ) const
{
  return mVram[physical( address & 0x7fffU )];
}

std::uint32_t Igs023::vramWord( std::size_t address ) const
{
  // As the layers read VRAM: the low byte at the even address.
  return static_cast<std::uint32_t>( vramAt( address ) ) |
         ( static_cast<std::uint32_t>( vramAt( address + 1 ) ) << 8U );
}

std::uint32_t Igs023::tileRom( std::uint32_t address ) const
{
  address &= 0xffffffU;
  if ( mTiles.cartridge && address >= mTiles.tileBase )
  {
    return mSdram.longWord( Sdram::CART_TILES_AT + ( address - mTiles.tileBase ) );
  }
  return mSdram.longWord( Sdram::BIOS_TILES_AT + address );
}

std::uint16_t Igs023::textPixel( std::uint32_t code, std::uint8_t attributes, std::uint32_t x, std::uint32_t y ) const
{
  // igs023_fg.sv: 8x8 tiles of 4 bits, a row of eight in one 32-bit word.
  bool const flipY = ( attributes & 0x80U ) != 0;
  bool const flipX = ( attributes & 0x40U ) != 0;
  std::uint32_t const row = flipY ? ( ~y & 7U ) : ( y & 7U );
  std::uint32_t const pixels = tileRom( ( code << 5U ) | ( row << 2U ) );
  std::uint32_t const pixel = flipX ? 7 - ( x & 7U ) : ( x & 7U );
  std::uint32_t const value = ( pixels >> ( pixel * 4 ) ) & 0xfU;
  return value == 0xf ? NONE
                      : static_cast<std::uint16_t>( TEXT_PALETTE + ( ( ( attributes >> 1U ) & 0x1fU ) << 4U ) + value );
}

std::uint16_t
Igs023::backgroundPixel( std::uint32_t code, std::uint8_t attributes, std::uint32_t x, std::uint32_t y ) const
{
  // igs023_bg.sv: 32x32 tiles of 5 bits. A row is a stream of 160 bits, five
  // pixels to a 5-bit group, lowest first, held in five 32-bit words.
  bool const flipY = ( attributes & 0x80U ) != 0;
  bool const flipX = ( attributes & 0x40U ) != 0;
  std::uint32_t const row = flipY ? ( ~y & 31U ) : ( y & 31U );
  std::uint32_t const pixel = flipX ? 31 - ( x & 31U ) : ( x & 31U );
  std::uint32_t const rowAddress = ( ( code << 5U ) | row ) * 20;
  std::uint32_t const bit = pixel * 5;
  std::uint64_t const words =
      tileRom( rowAddress + ( ( bit >> 5U ) * 4 ) ) |
      ( static_cast<std::uint64_t>( tileRom( rowAddress + ( ( ( bit >> 5U ) + 1 ) * 4 ) ) ) << 32U );
  std::uint32_t const value = static_cast<std::uint32_t>( words >> ( bit & 31U ) ) & 0x1fU;
  return value == 0x1f
             ? NONE
             : static_cast<std::uint16_t>( BACKGROUND_PALETTE + ( ( ( attributes >> 1U ) & 0x1fU ) << 5U ) + value );
}

void Igs023::textRow( std::uint32_t code,
                      std::uint8_t attributes,
                      std::uint32_t y,
                      std::array<std::uint16_t, 8>& out ) const
{
  bool const flipY = ( attributes & 0x80U ) != 0;
  bool const flipX = ( attributes & 0x40U ) != 0;
  std::uint32_t const row = flipY ? ( ~y & 7U ) : ( y & 7U );
  std::uint32_t const pixels = tileRom( ( code << 5U ) | ( row << 2U ) );
  auto const palette = static_cast<std::uint16_t>( TEXT_PALETTE + ( ( ( attributes >> 1U ) & 0x1fU ) << 4U ) );
  for ( std::uint32_t pixel = 0; pixel < 8; ++pixel )
  {
    std::uint32_t const value = ( pixels >> ( pixel * 4 ) ) & 0xfU;
    out.at( flipX ? 7 - pixel : pixel ) = static_cast<std::uint16_t>( palette + value );
  }
}

void Igs023::backgroundRow( std::uint32_t code,
                            std::uint8_t attributes,
                            std::uint32_t y,
                            std::array<std::uint16_t, 32>& out ) const
{
  bool const flipY = ( attributes & 0x80U ) != 0;
  bool const flipX = ( attributes & 0x40U ) != 0;
  std::uint32_t const row = flipY ? ( ~y & 31U ) : ( y & 31U );
  std::uint32_t const rowAddress = ( ( code << 5U ) | row ) * 20;
  // The row's 160 bits, and the word after them that backgroundPixel()
  // reads with the last group.
  std::array<std::uint32_t, 6> words{};
  for ( std::uint32_t i = 0; i < words.size(); ++i )
  {
    words.at( i ) = tileRom( rowAddress + ( i * 4 ) );
  }
  auto const palette = static_cast<std::uint16_t>( BACKGROUND_PALETTE + ( ( ( attributes >> 1U ) & 0x1fU ) << 5U ) );
  for ( std::uint32_t pixel = 0; pixel < 32; ++pixel )
  {
    std::uint32_t const bit = pixel * 5;
    std::uint64_t const pair =
        words.at( bit >> 5U ) | ( static_cast<std::uint64_t>( words.at( ( bit >> 5U ) + 1 ) ) << 32U );
    std::uint32_t const value = static_cast<std::uint32_t>( pair >> ( bit & 31U ) ) & 0x1fU;
    out.at( flipX ? 31 - pixel : pixel ) = static_cast<std::uint16_t>( palette + value );
  }
}

std::array<std::uint8_t, 4> Igs023::colour( std::uint32_t entry ) const
{
  // xRGB555, each 5-bit channel widened as PGM.sv widens it.
  std::size_t const at = std::size_t{ entry & 0xfffU } * 2;
  std::uint32_t const word = ( static_cast<std::uint32_t>( mPalette[at] ) << 8U ) | mPalette[at + 1];
  std::array<std::uint8_t, 4> rgba{ 0, 0, 0, 0xff };
  std::size_t channel = 0;
  for ( unsigned const shift : { 10U, 5U, 0U } )
  {
    std::uint32_t const value = ( word >> shift ) & 0x1fU;
    rgba.at( channel++ ) = static_cast<std::uint8_t>( ( value << 3U ) | ( value >> 2U ) );
  }
  return rgba;
}

void Igs023::drawText( int line, std::array<std::uint16_t, WIDTH>& out ) const
{
  // A 64x32 map from 0x4000, 4 bytes a tile.
  std::uint32_t const y = ( static_cast<std::uint32_t>( line ) + mControl[SCROLL_FG_Y] ) & 0xffU;
  std::uint32_t const x = mControl[SCROLL_FG_X] & 0x1ffU;
  std::uint32_t const rowBase = 0x4000U + ( ( y >> 3U ) << 8U );
  std::array<std::uint16_t, 8> tile{};
  for ( int i = 0; i < WIDTH; ++i )
  {
    std::uint32_t const at = ( x & 7U ) + static_cast<std::uint32_t>( i );
    if ( i == 0 || ( at & 7U ) == 0 )
    {
      std::uint32_t const column = ( ( x >> 3U ) + ( at >> 3U ) ) & 63U;
      std::uint32_t const entry = rowBase + ( column * 4 );
      textRow( vramWord( entry ), vramAt( entry + 2 ), y, tile );
    }
    out.at( static_cast<std::size_t>( i ) ) = tile.at( at & 7U );
  }
}

void Igs023::drawBackground( int line, std::array<std::uint16_t, WIDTH>& out ) const
{
  // A 64-column map at 0x0000 folded into 4 KB, and a scroll word per line at
  // 0x7000.
  std::uint32_t const y = ( static_cast<std::uint32_t>( line ) + mControl[SCROLL_BG_Y] ) & 0x7ffU;
  std::uint32_t const x = backgroundScrollX( line );
  std::uint32_t const rowBase = ( y >> 5U ) << 8U;
  std::array<std::uint16_t, 32> tile{};
  for ( int i = 0; i < WIDTH; ++i )
  {
    std::uint32_t const at = ( x & 31U ) + static_cast<std::uint32_t>( i );
    if ( i == 0 || ( at & 31U ) == 0 )
    {
      std::uint32_t const column = ( ( x >> 5U ) + ( at >> 5U ) ) & 63U;
      std::uint32_t const entry = rowBase + ( column * 4 );
      backgroundRow( vramWord( entry ) & 0x7fffU, vramAt( entry + 2 ), y, tile );
    }
    out.at( static_cast<std::size_t>( i ) ) = tile.at( at & 31U );
  }
}

void Igs023::drawLine( int line )
{
  // The flags register turns layers off. The background's pixels are still
  // fetched with it off, as the mixer may show them below.
  std::uint16_t const flags = controlFlags();
  bool const textOn = ( flags & TEXT_DISABLE ) == 0;
  bool const backgroundOn = ( flags & BACKGROUND_DISABLE ) == 0;
  bool const spritesOn = ( flags & SPRITES_DISABLE ) == 0;

  std::array<std::uint16_t, WIDTH> text{};
  std::array<std::uint16_t, WIDTH> background{};
  text.fill( NONE );
  background.fill( NONE );
  if ( mLayers.text && textOn )
  {
    drawText( line, text );
  }
  if ( mLayers.background )
  {
    drawBackground( line, background );
  }
  static SpriteLine const NO_SPRITES = []
  {
    SpriteLine erased{};
    erased.fill( SPRITE_ERASED );
    return erased;
  }();
  SpriteLine const& sprites = mLayers.sprites ? mSprites->at( static_cast<std::size_t>( line ) ) : NO_SPRITES;

  std::size_t at = static_cast<std::size_t>( line ) * WIDTH * 4;
  for ( std::size_t i = 0; i < WIDTH; ++i )
  {
    // FG over high-priority sprites over BG over low-priority sprites, as
    // igs023.sv mixes them. A pixel no sprite drew is a low-priority one of
    // the backdrop's colour; under a disabled high-priority sprite and a
    // transparent background, the background's pen shows.
    std::uint16_t const sprite = sprites[i];
    bool const spriteHigh = ( sprite & 0x400U ) == 0;
    bool const backgroundShown = backgroundOn && backgroundOpaque( background[i] );
    std::uint32_t word = 0;
    if ( textOpaque( text[i] ) )
    {
      word = text[i];
    }
    else if ( spriteHigh ? spritesOn : !backgroundShown )
    {
      word = SPRITE_PALETTE + ( sprite & 0x3ffU );
    }
    else if ( backgroundShown )
    {
      word = background[i];
    }
    else
    {
      word = background[i] == NONE ? BACKDROP : background[i];
    }

    for ( std::uint8_t const channel : colour( word ) )
    {
      mBuilding[at++] = channel;
    }
  }
}

void Igs023::setLayers( VideoLayers layers )
{
  mLayers = layers;
}

VideoLayers Igs023::layers() const
{
  return mLayers;
}

std::array<std::uint16_t, 16> const& Igs023::registers() const
{
  return mControl;
}

std::array<std::uint16_t, 32> const& Igs023::zoomTable() const
{
  return mZoomTable;
}

SpriteList const& Igs023::spriteList() const
{
  return mSpriteList;
}

namespace
{

/// Where a debugger's picture shows a transparent pixel.
constexpr std::array<std::uint8_t, 4> TRANSPARENT_GREY{ 0x20, 0x20, 0x20, 0xff };

void putPixel( Image& image, int x, int y, std::array<std::uint8_t, 4> const& rgba )
{
  std::size_t const at =
      ( ( static_cast<std::size_t>( y ) * static_cast<std::size_t>( image.width ) ) + static_cast<std::size_t>( x ) ) *
      4;
  std::ranges::copy( rgba, image.rgba.begin() + static_cast<std::ptrdiff_t>( at ) );
}

} // namespace

Image Igs023::tiles(
    TileLayer layer, std::uint32_t first, std::uint32_t count, std::uint32_t columns, std::uint32_t palette ) const
{
  bool const text = layer == TileLayer::TEXT;
  int const size = text ? 8 : 32;
  columns = std::clamp( columns, 1U, std::max( count, 1U ) );
  std::uint32_t const rows = ( count + columns - 1 ) / columns;
  Image image{ .width = static_cast<int>( columns ) * size, .height = static_cast<int>( rows ) * size, .rgba = {} };
  image.rgba.assign( static_cast<std::size_t>( image.width ) * static_cast<std::size_t>( image.height ) * 4, 0 );
  // The palette group sits in the attributes byte's bits 1-5.
  auto const attributes = static_cast<std::uint8_t>( ( palette & 0x1fU ) << 1U );
  for ( std::uint32_t i = 0; i < count; ++i )
  {
    std::uint32_t const code = first + i;
    int const left = static_cast<int>( i % columns ) * size;
    int const top = static_cast<int>( i / columns ) * size;
    for ( int y = 0; y < size; ++y )
    {
      for ( int x = 0; x < size; ++x )
      {
        auto const ux = static_cast<std::uint32_t>( x );
        auto const uy = static_cast<std::uint32_t>( y );
        std::uint16_t const entry =
            text ? textPixel( code, attributes, ux, uy ) : backgroundPixel( code & 0x7fffU, attributes, ux, uy );
        putPixel( image, left + x, top + y, entry == NONE ? TRANSPARENT_GREY : colour( entry ) );
      }
    }
  }
  return image;
}

Image Igs023::tilemap( TileLayer layer ) const
{
  bool const text = layer == TileLayer::TEXT;
  int const size = text ? 8 : 32;
  int const columns = 64;
  // The background's map is folded into 4 KB: 16 rows of 64, repeating.
  int const rows = text ? 32 : 16;
  Image image{ .width = columns * size, .height = rows * size, .rgba = {} };
  image.rgba.assign( static_cast<std::size_t>( image.width ) * static_cast<std::size_t>( image.height ) * 4, 0 );
  for ( int row = 0; row < rows; ++row )
  {
    for ( int column = 0; column < columns; ++column )
    {
      std::uint32_t const entry = ( text ? 0x4000U : 0U ) + ( static_cast<std::uint32_t>( row ) << 8U ) +
                                  ( static_cast<std::uint32_t>( column ) * 4 );
      std::uint32_t const code = text ? vramWord( entry ) : vramWord( entry ) & 0x7fffU;
      std::uint8_t const attributes = vramAt( entry + 2 );
      for ( int y = 0; y < size; ++y )
      {
        for ( int x = 0; x < size; ++x )
        {
          auto const ux = static_cast<std::uint32_t>( x );
          auto const uy = static_cast<std::uint32_t>( y );
          std::uint16_t const pixel =
              text ? textPixel( code, attributes, ux, uy ) : backgroundPixel( code, attributes, ux, uy );
          putPixel(
              image, ( column * size ) + x, ( row * size ) + y, pixel == NONE ? TRANSPARENT_GREY : colour( pixel ) );
        }
      }
    }
  }
  return image;
}

std::span<std::uint8_t const> Igs023::frame() const
{
  return mFrame;
}

std::int64_t Igs023::framesCompleted() const
{
  return mFramesCompleted;
}

Time Igs023::busHeldUntil() const
{
  return mBusHeldUntil;
}

void Igs023::reset()
{
  mIrq6 = false;
  mIrq4 = false;
}

bool Igs023::irq6() const
{
  return mIrq6;
}

bool Igs023::irq4() const
{
  return mIrq4;
}

std::uint16_t Igs023::controlFlags() const
{
  return mControl[FLAGS];
}

std::uint16_t Igs023::read( Time now, std::uint32_t address, bool upper, bool lower )
{
  switch ( ( address >> 20U ) & 0x3U )
  {
  case 1: // VRAM: the upper byte of a word sits at the odd physical address.
  {
    std::size_t const word = address & 0x7ffeU;
    std::uint16_t value = 0;
    if ( upper )
    {
      value = static_cast<std::uint16_t>( mVram[physical( word | 1U )] << 8U );
    }
    if ( lower )
    {
      value = static_cast<std::uint16_t>( value | mVram[physical( word )] );
    }
    return value;
  }
  case 2: // Palette: 4K words, mirrored through the megabyte.
  {
    std::size_t const at = address & 0x1ffeU;
    return static_cast<std::uint16_t>( ( mPalette[at] << 8U ) | mPalette[at + 1] );
  }
  case 3: // Registers: one per 4 KB, the zoom table in the second.
  {
    advanceTo( now );
    std::size_t const index = ( address >> 12U ) & 0xfU;
    return index == ZOOM_TABLE ? mZoomTable[( address >> 1U ) & 0x1fU] : mControl[index];
  }
  default:
    return 0;
  }
}

void Igs023::write( Time now, std::uint32_t address, std::uint16_t value, bool upper, bool lower )
{
  auto const merge = [&]( std::uint16_t& target )
  {
    if ( upper )
    {
      target = static_cast<std::uint16_t>( ( target & 0x00ffU ) | ( value & 0xff00U ) );
    }
    if ( lower )
    {
      target = static_cast<std::uint16_t>( ( target & 0xff00U ) | ( value & 0x00ffU ) );
    }
  };

  switch ( ( address >> 20U ) & 0x3U )
  {
  case 1:
  {
    std::size_t const word = address & 0x7ffeU;
    if ( upper )
    {
      mVram[physical( word | 1U )] = static_cast<std::uint8_t>( value >> 8U );
    }
    if ( lower )
    {
      mVram[physical( word )] = static_cast<std::uint8_t>( value );
    }
    return;
  }
  case 2:
  {
    std::size_t const at = address & 0x1ffeU;
    if ( upper )
    {
      mPalette[at] = static_cast<std::uint8_t>( value >> 8U );
    }
    if ( lower )
    {
      mPalette[at + 1] = static_cast<std::uint8_t>( value );
    }
    return;
  }
  case 3:
  {
    advanceTo( now );
    std::size_t const index = ( address >> 12U ) & 0xfU;
    merge( index == ZOOM_TABLE ? mZoomTable[( address >> 1U ) & 0x1fU] : mControl[index] );
    // An interrupt stays raised until its enable is cleared.
    if ( ( controlFlags() & IRQ6_ENABLE ) == 0 )
    {
      mIrq6 = false;
    }
    if ( ( controlFlags() & IRQ4_ENABLE ) == 0 )
    {
      mIrq4 = false;
    }
    return;
  }
  default:
    return;
  }
}

Time Igs023::vramFreeAt( Time now, bool write ) const
{
  if ( ( controlFlags() & CPU_BUS_MASTER ) != 0 )
  {
    return now;
  }

  // The access is followed through igs023.sv's arbiter a master tick at a
  // time, from the tick its request is pending on.
  std::int64_t const pending = ( now + UNITS_PER_MASTER_TICK - 1 ) / UNITS_PER_MASTER_TICK;

  // The lock that holds `tick`, as the tick it ends on, or -1. The text
  // layer's fetch and the background's head start are a tick apart.
  auto const lockedUntil = [&]( std::int64_t tick )
  {
    std::int64_t const line = dotOfTick( tick ) / DOTS_PER_LINE;
    for ( std::int64_t const fetch : { line, line - 1 } )
    {
      if ( fetch < 0 || !fetchedOn( fetch ) )
      {
        continue;
      }
      FetchLock const lock = fetchLock( fetch );
      if ( tick >= lock.textStart && tick < lock.textEnd )
      {
        return lock.textEnd;
      }
      if ( tick > lock.textEnd && tick < lock.end )
      {
        return lock.end;
      }
    }
    return std::int64_t{ -1 };
  };

  // The dot of the microcycle `tick` is in, counted from the last lock's end.
  auto const slotOf = [&]( std::int64_t tick )
  {
    std::int64_t const line = dotOfTick( tick ) / DOTS_PER_LINE;
    for ( std::int64_t fetch = line; fetch >= line - 2 && fetch >= 0; --fetch )
    {
      if ( fetchedOn( fetch ) )
      {
        std::int64_t const end = fetchLock( fetch ).end;
        if ( end <= tick )
        {
          return ( dotOfTick( tick ) - dotOfTick( end ) ) % MICROCYCLE_DOTS;
        }
      }
    }
    return std::int64_t{ 0 };
  };

  // Lines 39 to 263: from the first fetch to the end of the frame.
  auto const scheduled = [&]( std::int64_t tick )
  {
    std::int64_t const dot = dotOfTick( tick );
    auto const vcnt = static_cast<int>( ( dot / DOTS_PER_LINE ) % LINES_PER_FRAME );
    return vcnt > FIRST_FETCH_LINE || ( vcnt == FIRST_FETCH_LINE && dot % DOTS_PER_LINE == DOTS_PER_LINE - 1 );
  };

  enum class Phase : std::uint8_t
  {
    WAITING,
    LOW_SETUP,
    LOW_HOLD,
    HIGH_SETUP,
    HIGH_HOLD
  };
  Phase phase = Phase::WAITING;
  bool held = false;     // waited through a lock, so started at its end
  bool straggle = false; // a write started too late in the window
  bool freeBefore = lockedUntil( pending - 1 ) < 0;
  bool highFreeBefore = freeBefore;
  for ( std::int64_t tick = pending;; )
  {
    if ( std::int64_t const until = lockedUntil( tick ); until >= 0 )
    {
      held = held || phase == Phase::WAITING;
      tick = until;
      freeBefore = false;
      highFreeBefore = false;
      continue;
    }
    std::int64_t const slot = slotOf( tick );
    std::int64_t const subslot = ( tick - 1 ) % TICKS_PER_DOT;
    // A straggler's odd byte waits for the dot the background gives up.
    bool const highFree = !straggle || slot == 3;
    switch ( phase )
    {
    case Phase::WAITING:
      if ( !scheduled( tick ) || slot >= 4 || ( slot == 0 && subslot < 2 ) || held )
      {
        phase = Phase::LOW_SETUP;
        straggle = scheduled( tick ) && !held && write && slot == 7 && subslot >= 4;
      }
      break;
    case Phase::LOW_SETUP:
      phase = freeBefore ? Phase::LOW_HOLD : phase;
      break;
    case Phase::LOW_HOLD:
      phase = freeBefore ? Phase::HIGH_SETUP : phase;
      break;
    case Phase::HIGH_SETUP:
      phase = highFreeBefore ? Phase::HIGH_HOLD : phase;
      break;
    case Phase::HIGH_HOLD:
      if ( highFreeBefore )
      {
        return now + ( ( tick - pending - ACCESS_TICKS ) * UNITS_PER_MASTER_TICK );
      }
      break;
    }
    freeBefore = true;
    highFreeBefore = highFree;
    ++tick;
  }
}

int Igs023::waitStates( std::uint32_t address, bool write )
{
  bool const vram = ( ( address >> 20U ) & 0x3U ) == 1;
  if ( !vram )
  {
    return write ? 1 : 0;
  }
  return 2;
}

std::span<std::uint8_t const> Igs023::vram() const
{
  return mVram;
}

std::span<std::uint8_t const> Igs023::palette() const
{
  return mPalette;
}

int Igs023::line( Time now )
{
  return static_cast<int>( ( dotsAt( now ) / DOTS_PER_LINE ) % LINES_PER_FRAME );
}

int Igs023::dot( Time now )
{
  return static_cast<int>( dotsAt( now ) % DOTS_PER_LINE );
}

bool Igs023::vblank( Time now )
{
  return line( now ) < VBLANK_LINES;
}

bool Igs023::hblank( Time now )
{
  return dot( now ) < HBLANK_DOTS;
}

} // namespace pgm::machine
