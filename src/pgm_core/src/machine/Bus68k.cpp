#include "Bus68k.hpp"

namespace pgm::machine
{

namespace
{

constexpr std::uint32_t ADDRESS_MASK = 0xffffffU;

} // namespace

std::uint16_t RomSpace::word( std::uint32_t address ) const
{
  if ( cartridge && address >= cartBase )
  {
    return sdram->word( Sdram::CART_PROGRAM_AT + ( address - cartBase ) );
  }
  return sdram->word( Sdram::BIOS_PROGRAM_AT + address );
}

Bus68k::Bus68k( RomSpace rom, BusDevices devices, Time& time, std::span<std::uint8_t> workRam )
    : mRom{ rom }, mVideo{ devices.video }, mIo{ devices.io }, mAsic3{ devices.asic3 }, mInputs{ devices.inputs },
      mProtection{ devices.protection }, mTime{ time }, mWorkRam{ workRam }
{
  if ( mProtection != nullptr )
  {
    for ( std::uint8_t const page : mProtection->pages() )
    {
      mProtectionPages.at( page ) = true;
    }
  }
}

bool Bus68k::isProtection( std::uint32_t address ) const
{
  return mProtectionPages[address >> 16U] && mProtection->decodes( address );
}

std::uint16_t Bus68k::read( std::uint32_t address, bool upper, bool lower )
{
  address &= ADDRESS_MASK;
  if ( isProtection( address ) )
  {
    return mProtection->read( mTime, address, upper, lower );
  }
  switch ( address >> 20U )
  {
  case 0x0:
  case 0x1:
  case 0x2:
  case 0x3:
  case 0x4:
  case 0x5:
  case 0x6:
  case 0x7:
    return mRom.word( address & ~1U );
  case 0x8:
  {
    std::size_t const at = address & 0x1fffeU;
    return static_cast<std::uint16_t>( ( mWorkRam[at] << 8U ) | mWorkRam[at + 1] );
  }
  case 0x9:
  case 0xa:
  case 0xb:
  {
    if ( ( address >> 20U ) == 0x9 )
    {
      mTime = mVideo.vramFreeAt( mTime, false );
    }
    std::uint16_t const value = mVideo.read( mTime, address, upper, lower );
    mTime += Igs023::waitStates( address, false ) * UNITS_PER_M68K_CYCLE;
    return value;
  }
  case 0xc:
    if ( ( address & 0xffff00U ) == 0xc08000U )
    {
      // The inputs are active low.
      return static_cast<std::uint16_t>( ~mInputs.pressed[( address >> 1U ) & 0x3U] );
    }
    if ( ( address & 0xfffff0U ) == 0xc04000U )
    {
      return mAsic3.read();
    }
    if ( ( address & 0xfe0000U ) == 0xc00000U )
    {
      return mIo.read( mTime, address, upper, lower );
    }
    return 0;
  default:
    return 0;
  }
}

void Bus68k::write( std::uint32_t address, std::uint16_t value, bool upper, bool lower )
{
  address &= ADDRESS_MASK;
  if ( isProtection( address ) )
  {
    mProtection->write( mTime, address, value, upper, lower );
    return;
  }
  switch ( address >> 20U )
  {
  case 0x8:
  {
    std::size_t const at = address & 0x1fffeU;
    if ( upper )
    {
      mWorkRam[at] = static_cast<std::uint8_t>( value >> 8U );
    }
    if ( lower )
    {
      mWorkRam[at + 1] = static_cast<std::uint8_t>( value );
    }
    return;
  }
  case 0x9:
  case 0xa:
  case 0xb:
    if ( ( address >> 20U ) == 0x9 )
    {
      mTime = mVideo.vramFreeAt( mTime, true );
    }
    mVideo.write( mTime, address, value, upper, lower );
    mTime += Igs023::waitStates( address, true ) * UNITS_PER_M68K_CYCLE;
    return;
  case 0xc:
    if ( ( address & 0xffff00U ) == 0xc08000U )
    {
      return; // The inputs are read-only.
    }
    if ( ( address & 0xfffff0U ) == 0xc04000U )
    {
      mAsic3.write( address, value );
      return;
    }
    if ( ( address & 0xfe0000U ) == 0xc00000U )
    {
      mIo.write( mTime, address, value, upper, lower );
    }
    return;
  default:
    return; // ROM, and nothing.
  }
}

std::uint16_t Bus68k::peek( std::uint32_t address ) const
{
  address &= ADDRESS_MASK & ~1U;
  if ( isProtection( address ) )
  {
    return mProtection->peek( address );
  }
  if ( address < 0x800000U )
  {
    return mRom.word( address );
  }
  if ( ( address >> 20U ) == 0x8 )
  {
    std::size_t const at = address & 0x1fffeU;
    return static_cast<std::uint16_t>( ( mWorkRam[at] << 8U ) | mWorkRam[at + 1] );
  }
  return 0;
}

std::span<std::uint8_t const> Bus68k::workRam() const
{
  return mWorkRam;
}

} // namespace pgm::machine
