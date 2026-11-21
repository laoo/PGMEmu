#include "Methods.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pgm::control
{

namespace
{

/// The most one `memory.read` returns. Larger reads are made in pieces, so that
/// no single response line grows past a few megabytes of hex.
constexpr std::uint64_t MAX_READ = 0x100000;

std::string toHex( std::span<std::uint8_t const> bytes )
{
  static constexpr std::array<char, 16> DIGITS{ '0', '1', '2', '3', '4', '5', '6', '7',
                                                '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
  std::string hex;
  hex.reserve( bytes.size() * 2 );
  for ( std::uint8_t const byte : bytes )
  {
    hex.push_back( DIGITS[byte >> 4U] );
    hex.push_back( DIGITS[byte & 0x0fU] );
  }
  return hex;
}

/// The bytes `hex` spells, two digits each, or nothing if it spells none.
std::optional<std::vector<std::uint8_t>> fromHex( std::string const& hex )
{
  auto const digit = []( char c ) -> int
  {
    if ( c >= '0' && c <= '9' )
    {
      return c - '0';
    }
    if ( c >= 'a' && c <= 'f' )
    {
      return c - 'a' + 10;
    }
    if ( c >= 'A' && c <= 'F' )
    {
      return c - 'A' + 10;
    }
    return -1;
  };
  if ( hex.size() % 2 != 0 )
  {
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve( hex.size() / 2 );
  for ( std::size_t i = 0; i < hex.size(); i += 2 )
  {
    int const high = digit( hex[i] );
    int const low = digit( hex[i + 1] );
    if ( high < 0 || low < 0 )
    {
      return std::nullopt;
    }
    bytes.push_back( static_cast<std::uint8_t>( ( high << 4 ) | low ) );
  }
  return bytes;
}

/// The region that is the 68000's address space rather than a block of
/// memory: read and written through the bus's decode, as a debugger does.
constexpr std::string_view BUS_REGION = "M68K";
constexpr std::uint64_t BUS_SIZE = 0x1000000;

/// The region `params` name: the 68000's address space when they name none.
std::expected<std::string, Error> regionOf( Json const& params )
{
  if ( !params.contains( "region" ) )
  {
    return std::string{ BUS_REGION };
  }
  return requireString( params, "region" );
}

Error pastTheEnd( std::uint64_t size, std::uint64_t address, std::uint64_t regionSize, std::string_view name )
{
  return Error{ .code = "invalid_range",
                .message =
                    fmt::format( "{} bytes at {} run past the {} bytes of {}", size, address, regionSize, name ) };
}

/// The RAM a region name is, for memory.write.
std::optional<machine::Ram> ramNamed( std::string_view name )
{
  if ( name == "WORK_RAM" )
  {
    return machine::Ram::WORK;
  }
  if ( name == "VIDEO_RAM" )
  {
    return machine::Ram::VIDEO;
  }
  if ( name == "PALETTE_RAM" )
  {
    return machine::Ram::PALETTE;
  }
  if ( name == "AUDIO_RAM" )
  {
    return machine::Ram::AUDIO;
  }
  return std::nullopt;
}

} // namespace

void addMemoryMethods( Dispatcher& dispatcher, Emulator& emulator )
{
  dispatcher.add( "memory.list_regions",
                  info( "The names of the memory regions memory.read can read now." ),
                  [&emulator]( Json const& /*params*/ ) -> Outcome
                  {
                    Json names = Json::array();
                    for ( MemoryRegion const& region : emulator.memoryRegions() )
                    {
                      names.push_back( region.name );
                    }
                    if ( emulator.machine() != nullptr )
                    {
                      names.push_back( BUS_REGION );
                    }
                    return names;
                  } );

  dispatcher.add(
      "memory.read",
      info( "Reads bytes of a memory region, at most 1 MB, answered as lowercase hex. Work RAM is in 68000 byte order: "
            "the high byte of a word first. Without a region, address is one of the 68000's, such as 0x810000.",
            { { .name = "region",
                .type = "string",
                .description = "A name from memory.list_regions, such as WORK_RAM, VIDEO_RAM, PALETTE_RAM or "
                               "AUDIO_RAM; M68K, the 68000's address space, if left out.",
                .required = false },
              { .name = "address",
                .type = "integer",
                .description = "The first byte's offset in the region; in M68K, its address." },
              { .name = "size", .type = "integer", .description = "How many bytes, at most 1048576." } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        auto const name = regionOf( params );
        if ( !name )
        {
          return std::unexpected( name.error() );
        }
        auto const address = requireUnsigned( params, "address" );
        if ( !address )
        {
          return std::unexpected( address.error() );
        }
        auto const size = requireUnsigned( params, "size" );
        if ( !size )
        {
          return std::unexpected( size.error() );
        }
        if ( *size > MAX_READ )
        {
          return std::unexpected(
              badRequest( fmt::format( "size {} is above the limit of {} bytes", *size, MAX_READ ) ) );
        }

        if ( *name == BUS_REGION )
        {
          machine::Machine const* const machine = emulator.machine();
          if ( machine == nullptr )
          {
            return std::unexpected( Error{ .code = "not_loaded", .message = "No game is loaded" } );
          }
          if ( *address > BUS_SIZE || *size > BUS_SIZE - *address )
          {
            return std::unexpected( pastTheEnd( *size, *address, BUS_SIZE, *name ) );
          }
          auto const bytes =
              machine->readBus( static_cast<std::uint32_t>( *address ), static_cast<std::size_t>( *size ) );
          return Json{ { "region", *name }, { "address", *address }, { "data_hex", toHex( bytes ) } };
        }

        auto const regions = emulator.memoryRegions();
        auto const region = std::ranges::find( regions, *name, &MemoryRegion::name );
        if ( region == regions.end() )
        {
          return std::unexpected(
              Error{ .code = "invalid_region", .message = fmt::format( "No memory region {} is loaded", *name ) } );
        }
        if ( *address > region->bytes.size() || *size > region->bytes.size() - *address )
        {
          return std::unexpected( pastTheEnd( *size, *address, region->bytes.size(), *name ) );
        }

        auto const bytes =
            region->bytes.subspan( static_cast<std::size_t>( *address ), static_cast<std::size_t>( *size ) );
        return Json{ { "region", *name }, { "address", *address }, { "data_hex", toHex( bytes ) } };
      } );

  dispatcher.add(
      "memory.write",
      info( "Writes bytes into a RAM, as a debugger does: no bus cycle is made. The bytes are laid out as memory.read "
            "reads them; work RAM in 68000 byte order. The whole range is checked first, so the write is made whole "
            "or not at all. Without a region, address is one of the 68000's, where RAM is mapped.",
            { { .name = "region",
                .type = "string",
                .description = "WORK_RAM, VIDEO_RAM, PALETTE_RAM or AUDIO_RAM; M68K, the 68000's address space, if "
                               "left out. The ROMs cannot be written.",
                .required = false },
              { .name = "address",
                .type = "integer",
                .description = "The first byte's offset in the region; in M68K, its address." },
              { .name = "data_hex", .type = "string", .description = "The bytes, two hex digits each." } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        auto const name = regionOf( params );
        if ( !name )
        {
          return std::unexpected( name.error() );
        }
        auto const address = requireUnsigned( params, "address" );
        if ( !address )
        {
          return std::unexpected( address.error() );
        }
        auto const hex = requireString( params, "data_hex" );
        if ( !hex )
        {
          return std::unexpected( hex.error() );
        }
        auto const bytes = fromHex( *hex );
        if ( !bytes )
        {
          return std::unexpected( badRequest( "data_hex must be pairs of hex digits" ) );
        }
        if ( bytes->size() > MAX_READ )
        {
          return std::unexpected(
              badRequest( fmt::format( "{} bytes is above the limit of {} bytes", bytes->size(), MAX_READ ) ) );
        }

        machine::Machine* const machine = emulator.machine();
        if ( machine == nullptr )
        {
          return std::unexpected( Error{ .code = "not_loaded", .message = "No game is loaded" } );
        }
        if ( *name == BUS_REGION )
        {
          if ( *address > BUS_SIZE || bytes->size() > BUS_SIZE - *address )
          {
            return std::unexpected( pastTheEnd( bytes->size(), *address, BUS_SIZE, *name ) );
          }
          if ( !machine->writeBus( static_cast<std::uint32_t>( *address ), *bytes ) )
          {
            return std::unexpected( Error{
                .code = "invalid_range",
                .message = fmt::format(
                    "{} bytes at {} do not all fall in work RAM, VRAM or the palette", bytes->size(), *address ) } );
          }
          return Json{ { "region", *name }, { "address", *address }, { "size", bytes->size() } };
        }
        auto const ram = ramNamed( *name );
        if ( !ram )
        {
          return std::unexpected(
              Error{ .code = "invalid_region",
                     .message = fmt::format(
                         "{} cannot be written; memory.write writes WORK_RAM, VIDEO_RAM, PALETTE_RAM and AUDIO_RAM",
                         *name ) } );
        }
        if ( !machine->writeRam( *ram, static_cast<std::size_t>( *address ), *bytes ) )
        {
          return std::unexpected( Error{
              .code = "invalid_range",
              .message = fmt::format( "{} bytes at {} run past the end of {}", bytes->size(), *address, *name ) } );
        }
        return Json{ { "region", *name }, { "address", *address }, { "size", bytes->size() } };
      } );
}

} // namespace pgm::control
