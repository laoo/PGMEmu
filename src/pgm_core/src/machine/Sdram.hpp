#pragma once

// The MiSTer core's SDRAM, as its devices read it: every ROM at the place
// system_consts.sv gives it at MiSTer core commit 6f757e4, and zero wherever
// nothing was loaded. The 68000's ROM space and the IGS023's tile fetches both
// reach past the ROMs they mean to read, and what they find there is this
// layout (docs/hardware/differences.md).

#include <cstdint>
#include <span>

namespace pgm::machine
{

struct Sdram
{
  // system_consts.sv
  static constexpr std::uint32_t BIOS_PROGRAM_AT = 0x000000;
  static constexpr std::uint32_t BIOS_TILES_AT = 0x100000;
  static constexpr std::uint32_t BIOS_MUSIC_AT = 0x300000;
  static constexpr std::uint32_t CART_PROGRAM_AT = 0x0800000;
  static constexpr std::uint32_t CART_TILES_AT = 0x1000000;
  static constexpr std::uint32_t CART_MUSIC_AT = 0x2000000;
  static constexpr std::uint32_t CART_B_ROM_AT = 0x3000000;
  static constexpr std::uint32_t CART_A_ROM_AT = 0x4000000;

  std::span<std::uint8_t const> biosProgram;
  std::span<std::uint8_t const> biosTiles;
  std::span<std::uint8_t const> biosMusic;
  std::span<std::uint8_t const> cartProgram;
  std::span<std::uint8_t const> cartTiles;
  std::span<std::uint8_t const> cartMusic;
  std::span<std::uint8_t const> cartBRom;
  std::span<std::uint8_t const> cartARom;

  /// The byte at `address`.
  [[nodiscard]] std::uint8_t byte( std::uint32_t address ) const;

  /// The 16-bit word at even `address`, its low byte first as the ROM files
  /// hold it.
  [[nodiscard]] std::uint16_t word( std::uint32_t address ) const;

  /// The 32-bit word at `address`, lowest byte first.
  [[nodiscard]] std::uint32_t longWord( std::uint32_t address ) const;
};

/// The bytes of the region holding `address`, from `address` on; empty where
/// nothing was loaded. Inline, as every ROM read of the machine comes here.
inline std::span<std::uint8_t const> sdramFrom( Sdram const& sdram, std::uint32_t address )
{
  auto const within = [address]( std::uint32_t at, std::span<std::uint8_t const> bytes )
  {
    std::uint32_t const offset = address - at;
    return offset < bytes.size() ? bytes.subspan( offset ) : std::span<std::uint8_t const>{};
  };
  // Highest first: every region starts above the end of the one before.
  if ( address >= Sdram::CART_A_ROM_AT )
  {
    return within( Sdram::CART_A_ROM_AT, sdram.cartARom );
  }
  if ( address >= Sdram::CART_B_ROM_AT )
  {
    return within( Sdram::CART_B_ROM_AT, sdram.cartBRom );
  }
  if ( address >= Sdram::CART_MUSIC_AT )
  {
    return within( Sdram::CART_MUSIC_AT, sdram.cartMusic );
  }
  if ( address >= Sdram::CART_TILES_AT )
  {
    return within( Sdram::CART_TILES_AT, sdram.cartTiles );
  }
  if ( address >= Sdram::CART_PROGRAM_AT )
  {
    return within( Sdram::CART_PROGRAM_AT, sdram.cartProgram );
  }
  if ( address >= Sdram::BIOS_MUSIC_AT )
  {
    return within( Sdram::BIOS_MUSIC_AT, sdram.biosMusic );
  }
  if ( address >= Sdram::BIOS_TILES_AT )
  {
    return within( Sdram::BIOS_TILES_AT, sdram.biosTiles );
  }
  return within( Sdram::BIOS_PROGRAM_AT, sdram.biosProgram );
}

inline std::uint8_t Sdram::byte( std::uint32_t address ) const
{
  auto const bytes = sdramFrom( *this, address );
  return bytes.empty() ? 0 : bytes[0];
}

inline std::uint16_t Sdram::word( std::uint32_t address ) const
{
  auto const bytes = sdramFrom( *this, address );
  if ( bytes.size() >= 2 )
  {
    return static_cast<std::uint16_t>( bytes[0] | ( bytes[1] << 8U ) );
  }
  return static_cast<std::uint16_t>( byte( address ) | ( byte( address + 1 ) << 8U ) );
}

inline std::uint32_t Sdram::longWord( std::uint32_t address ) const
{
  auto const bytes = sdramFrom( *this, address );
  if ( bytes.size() >= 4 )
  {
    return static_cast<std::uint32_t>( bytes[0] ) | ( static_cast<std::uint32_t>( bytes[1] ) << 8U ) |
           ( static_cast<std::uint32_t>( bytes[2] ) << 16U ) | ( static_cast<std::uint32_t>( bytes[3] ) << 24U );
  }
  return static_cast<std::uint32_t>( word( address ) ) | ( static_cast<std::uint32_t>( word( address + 2 ) ) << 16U );
}

} // namespace pgm::machine
