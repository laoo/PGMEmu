#pragma once

// The IGS023 video chip, ported from rtl/igs023.sv, igs023_fg.sv and
// igs023_bg.sv at MiSTer core commit e898860: its registers, VRAM, palette RAM,
// raster timing, line counter and interrupts, and the picture it draws, mixed
// as at commit 6f757e4. The sprites are SpriteEngine's.
//
// A line is drawn whole when the RTL starts fetching it, at dot 638 of the line
// before, from the registers and VRAM of that moment. The RTL's layers fetch
// over the next few microseconds, and its palette is read dot by dot, so a
// change in between shows on the RTL a line later or within the line; nothing
// seen so far relies on that.

#include "Sdram.hpp"
#include "SpriteEngine.hpp"

#include "pgm/machine/Time.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pgm::machine
{

/// Where the chip's tile fetches find the cartridge's tiles: at `tileBase` and
/// above in its address space, the BIOS's below (PGM.sv).
struct TileMapping
{
  bool cartridge{};
  std::uint32_t tileBase{};
};

class Igs023
{
public:
  static constexpr std::size_t VRAM_SIZE = 0x8000;
  static constexpr std::size_t PALETTE_SIZE = 0x2000;
  static constexpr int WIDTH = 448;
  static constexpr int HEIGHT = 224;

  /// `workRam` is what sprite DMA reads; `sdram` holds the tiles and sprites.
  Igs023( Sdram const& sdram, TileMapping tiles, std::span<std::uint8_t const> workRam );

  /// What the reset line does to the chip: drops both interrupts. Registers,
  /// RAM and the raster are untouched, as igs023.sv leaves them.
  void reset();

  /// Brings the raster up to `now`: the line counter, sprite DMA's trigger
  /// and the interrupts it raises on the way.
  void advanceTo( Time now );

  /// Whether interrupt 6 (vertical blank) and 4 (every 62 lines) are raised.
  [[nodiscard]] bool irq6() const;
  [[nodiscard]] bool irq4() const;

  /// A 68000 word read or write at byte address `address` in 0x900000-0xBFFFFF.
  /// `upper` and `lower` are the data strobes: which bytes take part.
  std::uint16_t read( Time now, std::uint32_t address, bool upper, bool lower );
  void write( Time now, std::uint32_t address, std::uint16_t value, bool upper, bool lower );

  /// The 68000 cycles a bus cycle to the chip waits for its DTACK.
  ///
  /// VRAM is 8 bits wide, and igs023.sv moves a word through it a byte at a
  /// time, two 50 MHz clocks a byte, before it acknowledges: DTACK comes about
  /// six master ticks after the chip select for a word and four for a byte.
  /// Registers and palette RAM acknowledge on the clock after it. The 68000
  /// samples DTACK at the end of S4, and asserts its data strobes at S2 for a
  /// read but S4 for a write, so a write waits longer for the same delay.
  /// Contention with the layers' own VRAM fetches, which the RTL arbitrates,
  /// is not counted until the layers are emulated (M3).
  [[nodiscard]] static int waitStates( std::uint32_t address, bool write, bool upper, bool lower );

  /// When the 68000 may next have VRAM: `now`, or the end of the text layer's
  /// fetch if one holds VRAM at `now`. igs023.sv gives VRAM to that fetch from
  /// dot 638 of each line before a visible one, for 464 cycles of the 33 MHz
  /// clock, unless the CPU bus-master flag (register 14, bit 10) is set. The
  /// background's reads, a few dots per 32-pixel tile, are not counted.
  [[nodiscard]] Time vramFreeAt( Time now ) const;

  /// VRAM as the chip's 8-bit RAM holds it, and palette RAM in the 68000's
  /// byte order: what the RTL simulator's VIDEO_RAM and PALETTE_RAM are.
  [[nodiscard]] std::span<std::uint8_t const> vram() const;
  [[nodiscard]] std::span<std::uint8_t const> palette() const;

  /// The last complete frame, RGBA, 448 by 224, and how many have completed.
  [[nodiscard]] std::span<std::uint8_t const> frame() const;
  [[nodiscard]] std::int64_t framesCompleted() const;

  /// Until when sprite DMA holds the 68000 off the bus; in the past when it
  /// does not.
  [[nodiscard]] Time busHeldUntil() const;

  /// What a debugger looks at: the layers drawn, the registers, the sprite
  /// list sprite DMA last copied, and the tiles and tile maps as images.
  void setLayers( VideoLayers layers );
  [[nodiscard]] VideoLayers layers() const;
  [[nodiscard]] std::array<std::uint16_t, 16> const& registers() const;
  [[nodiscard]] std::array<std::uint16_t, 32> const& zoomTable() const;
  [[nodiscard]] SpriteList const& spriteList() const;
  [[nodiscard]] Image tiles(
      TileLayer layer, std::uint32_t first, std::uint32_t count, std::uint32_t columns, std::uint32_t palette ) const;
  [[nodiscard]] Image tilemap( TileLayer layer ) const;

  /// Raster position at `now`, for conditions and the debugger.
  [[nodiscard]] static int line( Time now );
  [[nodiscard]] static int dot( Time now );
  [[nodiscard]] static bool vblank( Time now );
  [[nodiscard]] static bool hblank( Time now );

  /// Names its state for a save state (StateArchive.hpp).
  template <class Archive>
  void serialize( Archive& archive )
  {
    archive( mControl );
    archive( mZoomTable );
    archive( mVram );
    archive( mPalette );
    archive( mIrq6 );
    archive( mIrq4 );
    archive( mIrq4Count );
    archive( mEventsDone );
    archive( *mSprites );
    archive( *mNextSprites );
    archive( mNextSpritesReady );
    archive( mSpriteList );
    archive( mBusHeldUntil );
    archive( mBuilding );
    archive( mFrame );
    archive( mFramesCompleted );
  }

private:
  void onLineStart( int line );
  void onHsync( Time at );
  void onFetch( int line );
  [[nodiscard]] std::uint16_t controlFlags() const;

  /// Draws logical line `line`, 0 to 223, into the frame being built.
  void drawLine( int line );
  void drawText( int line, std::array<std::uint16_t, WIDTH>& out ) const;
  /// The palette entry pixel (x, y) of a tile takes, or NONE where it is
  /// transparent; the attributes byte's flips are applied. Text tiles are 8 by
  /// 8 of 4 bits, background tiles 32 by 32 of 5.
  [[nodiscard]] std::uint16_t
  textPixel( std::uint32_t code, std::uint8_t attributes, std::uint32_t x, std::uint32_t y ) const;
  [[nodiscard]] std::uint16_t
  backgroundPixel( std::uint32_t code, std::uint8_t attributes, std::uint32_t x, std::uint32_t y ) const;
  /// A tile's row `y` as textPixel() and backgroundPixel() give its pixels,
  /// from its left edge on screen: the tile ROM read once for all of them.
  void textRow( std::uint32_t code, std::uint8_t attributes, std::uint32_t y, std::array<std::uint16_t, 8>& out ) const;
  void backgroundRow( std::uint32_t code,
                      std::uint8_t attributes,
                      std::uint32_t y,
                      std::array<std::uint16_t, 32>& out ) const;
  /// Palette entry `entry` as R, G, B, A.
  [[nodiscard]] std::array<std::uint8_t, 4> colour( std::uint32_t entry ) const;
  void drawBackground( int line, std::array<std::uint16_t, WIDTH>& out ) const;
  [[nodiscard]] std::uint32_t tileRom( std::uint32_t address ) const;
  [[nodiscard]] std::uint8_t vramAt( std::size_t address ) const;
  [[nodiscard]] std::uint32_t vramWord( std::size_t address ) const;

  std::array<std::uint16_t, 16> mControl{};
  std::array<std::uint16_t, 32> mZoomTable{};
  std::array<std::uint8_t, VRAM_SIZE> mVram{};
  std::array<std::uint8_t, PALETTE_SIZE> mPalette{};
  bool mIrq6{};
  bool mIrq4{};
  std::uint8_t mIrq4Count{};
  /// The raster events up to here have been applied: each line has two, the
  /// start of its first dot and the rising edge of its hsync.
  std::int64_t mEventsDone{};

  Sdram const& mSdram;
  TileMapping mTiles;
  std::span<std::uint8_t const> mWorkRam;
  std::unique_ptr<SpriteFrame> mSprites;
  std::unique_ptr<SpriteFrame> mNextSprites;
  bool mNextSpritesReady{};
  SpriteList mSpriteList;
  Time mBusHeldUntil{};
  std::vector<std::uint8_t> mBuilding;
  std::vector<std::uint8_t> mFrame;
  std::int64_t mFramesCompleted{};
  VideoLayers mLayers;
};

} // namespace pgm::machine
