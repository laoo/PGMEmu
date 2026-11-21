#include "pgm/Emulator.hpp"

#include "pgm/io/RomSources.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>
#include <utility>

namespace pgm
{

namespace
{

LoadFailure loadFailed( std::string message )
{
  return LoadFailure{ .kind = LoadFailure::Kind::LOAD_FAILED, .message = std::move( message ) };
}

bool isSetName( std::string_view name )
{
  return !name.empty() &&
         std::ranges::all_of(
             name, []( char c ) { return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_'; } );
}

// The region names the RTL simulator's memory.read accepts, so that the same
// request reads the same bytes from both. The cartridge's ARM internal ROM and
// IGS022 data ROM have no name there and are named here in the same style.
struct CartRegion
{
  std::string_view name;
  cart::RomType type;
};

constexpr std::array<CartRegion, 8> CART_REGIONS{ { { .name = "CART_PROG_ROM", .type = cart::RomType::PRG },
                                                    { .name = "CART_TILE_ROM", .type = cart::RomType::TLE },
                                                    { .name = "CART_MUSIC_ROM", .type = cart::RomType::AUD },
                                                    { .name = "CART_A_ROM", .type = cart::RomType::SPC },
                                                    { .name = "CART_B_ROM", .type = cart::RomType::SPM },
                                                    { .name = "CART_ARM_ROM", .type = cart::RomType::EXT },
                                                    { .name = "CART_ARM_INT_ROM", .type = cart::RomType::INT },
                                                    { .name = "CART_IGS022_ROM", .type = cart::RomType::I22 } } };

} // namespace

Emulator::Emulator( Settings settings ) : mSettings{ std::move( settings ) } {}

Settings const& Emulator::settings() const
{
  return mSettings;
}

bool Emulator::paused() const
{
  return mPaused;
}

void Emulator::setPaused( bool paused )
{
  mPaused = paused;
}

void Emulator::setBiosSources( std::vector<std::filesystem::path> sources )
{
  mSettings.biosSources = std::move( sources );
}

std::expected<void, LoadFailure> Emulator::loadGameByName( std::string_view name,
                                                           std::optional<std::string_view> region )
{
  if ( !isSetName( name ) )
  {
    return std::unexpected( LoadFailure{ .kind = LoadFailure::Kind::UNKNOWN_GAME,
                                         .message = fmt::format( "'{}' is not a set name", name ) } );
  }
  if ( name == BIOS_ONLY )
  {
    if ( region )
    {
      return std::unexpected( LoadFailure{ .kind = LoadFailure::Kind::UNKNOWN_REGION,
                                           .message = "the BIOS alone has no regions to choose from" } );
    }
    auto bios = loadBios();
    if ( !bios )
    {
      return std::unexpected( bios.error() );
    }
    install( std::move( *bios ), std::nullopt, std::nullopt );
    return {};
  }

  std::filesystem::path const path = mSettings.romDirectory / fmt::format( "{}.pgm", name );
  std::error_code failed;
  if ( !std::filesystem::is_regular_file( path, failed ) )
  {
    return std::unexpected(
        LoadFailure{ .kind = LoadFailure::Kind::UNKNOWN_GAME,
                     .message = fmt::format( "no {}.pgm in {}", name, mSettings.romDirectory.string() ) } );
  }
  return loadGameFromFile( path, region );
}

std::expected<void, LoadFailure> Emulator::loadGameFromFile( std::filesystem::path const& path,
                                                             std::optional<std::string_view> region )
{
  auto cartridge = cart::PgmImage::read( path );
  if ( !cartridge )
  {
    return std::unexpected( loadFailed( fmt::format( "{}: {}", path.string(), cartridge.error() ) ) );
  }
  std::optional<std::uint32_t> agnosticId;
  if ( region )
  {
    auto const chosen = chooseRegion( *cartridge, *region );
    if ( !chosen )
    {
      return std::unexpected( chosen.error() );
    }
    agnosticId = *chosen;
  }
  auto bios = loadBios();
  if ( !bios )
  {
    return std::unexpected( bios.error() );
  }
  install( std::move( *bios ), std::move( *cartridge ), agnosticId );
  return {};
}

std::expected<void, LoadFailure> Emulator::setRegion( std::string_view region )
{
  if ( !mCartridge )
  {
    return std::unexpected(
        LoadFailure{ .kind = LoadFailure::Kind::UNKNOWN_REGION,
                     .message = mBios ? "the BIOS alone has no regions to choose from" : "no game is loaded" } );
  }
  auto const chosen = chooseRegion( *mCartridge, region );
  if ( !chosen )
  {
    return std::unexpected( chosen.error() );
  }
  mRegion = *chosen;
  powerUp();
  return {};
}

std::expected<std::uint32_t, LoadFailure> Emulator::chooseRegion( cart::PgmImage const& cartridge,
                                                                  std::string_view region )
{
  auto const agnosticId = cart::agnosticIdOf( region );
  if ( !agnosticId )
  {
    return std::unexpected(
        LoadFailure{ .kind = LoadFailure::Kind::UNKNOWN_REGION,
                     .message = fmt::format( "'{}' is not a four-character region code", region ) } );
  }
  if ( auto const value = cartridge.regionValue( *agnosticId ); !value )
  {
    return std::unexpected( LoadFailure{ .kind = LoadFailure::Kind::UNKNOWN_REGION, .message = value.error() } );
  }
  return *agnosticId;
}

void Emulator::install( cart::Bios bios, std::optional<cart::PgmImage> cartridge, std::optional<std::uint32_t> region )
{
  // The machine reads the ROMs in place, so it goes before they are replaced.
  mMachine.reset();
  mBios = std::move( bios );
  mCartridge = std::move( cartridge );
  mRegion = region;
  powerUp();
}

void Emulator::powerUp()
{
  mMachine.reset();
  if ( !mBios )
  {
    return;
  }
  // A chosen region was checked against the cartridge when it was chosen.
  std::optional<std::uint32_t> value;
  if ( mCartridge && mRegion )
  {
    if ( auto const chosen = mCartridge->regionValue( *mRegion ) )
    {
      value = *chosen;
    }
  }
  mMachine = std::make_unique<machine::Machine>( *mBios, mCartridge ? &*mCartridge : nullptr, value );
  if ( mTraceSize != machine::Machine::TRACE_SIZE )
  {
    mMachine->setTraceSize( mTraceSize );
  }
  mMachine->setTraceRegisters( mTraceRegisters );
}

bool Emulator::traceRegisters() const
{
  return mTraceRegisters;
}

void Emulator::setTraceRegisters( bool kept )
{
  mTraceRegisters = kept;
  if ( mMachine )
  {
    mMachine->setTraceRegisters( kept );
  }
}

std::size_t Emulator::traceSize() const
{
  return mTraceSize;
}

void Emulator::setTraceSize( std::size_t instructions )
{
  mTraceSize = std::clamp<std::size_t>( instructions, 1, machine::Machine::MAX_TRACE_SIZE );
  if ( mMachine )
  {
    mMachine->setTraceSize( mTraceSize );
  }
}

machine::Machine* Emulator::machine()
{
  return mMachine.get();
}

machine::Machine const* Emulator::machine() const
{
  return mMachine.get();
}

std::optional<std::string> Emulator::gameName() const
{
  if ( mCartridge )
  {
    return mCartridge->shortName();
  }
  if ( mBios )
  {
    return std::string{ BIOS_ONLY };
  }
  return std::nullopt;
}

std::optional<std::string> Emulator::region() const
{
  if ( !mCartridge || !mCartridge->regionInfo() )
  {
    return std::nullopt;
  }
  if ( mRegion )
  {
    return cart::fourCc( *mRegion );
  }
  auto const own = mCartridge->ownRegion();
  if ( !own )
  {
    return std::nullopt;
  }
  auto const& regions = mCartridge->regionInfo()->regions;
  auto const region = std::ranges::find( regions, *own, &cart::Region::regionId );
  if ( region == regions.end() )
  {
    return std::nullopt;
  }
  return cart::fourCc( region->agnosticId );
}

cart::Bios const* Emulator::bios() const
{
  return mBios ? &*mBios : nullptr;
}

cart::PgmImage const* Emulator::cartridge() const
{
  return mCartridge ? &*mCartridge : nullptr;
}

std::vector<MemoryRegion> Emulator::memoryRegions() const
{
  std::vector<MemoryRegion> regions;
  if ( mBios )
  {
    regions.push_back( MemoryRegion{ .name = "BIOS_PROG_ROM", .bytes = mBios->program().data } );
    regions.push_back( MemoryRegion{ .name = "BIOS_TILE_ROM", .bytes = mBios->tiles().data } );
    regions.push_back( MemoryRegion{ .name = "BIOS_MUSIC_ROM", .bytes = mBios->music().data } );
  }
  if ( mMachine )
  {
    regions.push_back( MemoryRegion{ .name = "WORK_RAM", .bytes = mMachine->workRam() } );
    regions.push_back( MemoryRegion{ .name = "VIDEO_RAM", .bytes = mMachine->videoRam() } );
    regions.push_back( MemoryRegion{ .name = "PALETTE_RAM", .bytes = mMachine->paletteRam() } );
    regions.push_back( MemoryRegion{ .name = "AUDIO_RAM", .bytes = mMachine->z80Ram() } );
  }
  if ( mCartridge )
  {
    for ( CartRegion const& region : CART_REGIONS )
    {
      if ( auto const rom = mCartridge->rom( region.type ) )
      {
        regions.push_back( MemoryRegion{ .name = region.name, .bytes = rom->data } );
      }
    }
  }
  return regions;
}

std::expected<cart::Bios, LoadFailure> Emulator::loadBios() const
{
  if ( mSettings.biosSources.empty() )
  {
    return std::unexpected( loadFailed( "no BIOS source is configured" ) );
  }
  auto sources = io::RomSources::open( mSettings.biosSources );
  if ( !sources )
  {
    return std::unexpected( loadFailed( sources.error() ) );
  }
  auto bios = cart::Bios::load( *sources );
  if ( !bios )
  {
    return std::unexpected( loadFailed( bios.error() ) );
  }
  return std::move( *bios );
}

} // namespace pgm
