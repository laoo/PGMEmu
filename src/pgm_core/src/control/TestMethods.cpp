#include "Methods.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pgm::control
{

namespace
{

// PGMTest's debug link as the RTL simulator serves it: a block in work RAM the
// test ROM publishes, "RFIF", with a ring each way (PGMTest's debug_link.c,
// the simulator's sim_core.cpp). The ring indices are 68000 words.
constexpr std::array<std::uint8_t, 4> LINK_MAGIC{ 'R', 'F', 'I', 'F' };
constexpr std::size_t LINK_ACTIVE = 4;
constexpr std::size_t LINK_TARGET_READY = 5;
constexpr std::size_t LINK_IN_HEAD = 6;
constexpr std::size_t LINK_IN_TAIL = 8;
constexpr std::size_t LINK_OUT_HEAD = 10;
constexpr std::size_t LINK_OUT_TAIL = 12;
constexpr std::size_t LINK_IN_BUFFER = 14;
constexpr std::size_t LINK_RING = 512;
constexpr std::size_t LINK_OUT_BUFFER = LINK_IN_BUFFER + LINK_RING;
constexpr std::size_t LINK_BLOCK = LINK_OUT_BUFFER + LINK_RING;

/// How far the machine runs between two looks at the rings, in master ticks,
/// as the simulator steps it.
constexpr std::int64_t ATTACH_STEP = 50'000;
constexpr std::int64_t RING_STEP = 2048;
constexpr std::uint64_t DEFAULT_TIMEOUT = 2'000'000;

/// Runs `machine` a step and answers the time it counts for. A halted 68000
/// runs no time at all, and a wait would never end, so a step counts whole.
std::int64_t step( machine::Machine& machine, std::int64_t ticks )
{
  return std::max( machine.runTicks( ticks ).ticks, ticks );
}

/// Where PGMTest's pages publish their state: the last 4 KB of work RAM,
/// 0x81F000, a 16-bit magic and then words of the page's own.
constexpr std::size_t STATUS_AT = 0x1f000;
constexpr std::uint64_t STATUS_WORDS = 2048;

/// The link's state between requests, and the machine it is attached to.
struct Link
{
  machine::Machine const* machine{};
  std::optional<std::size_t> base;
  std::deque<std::uint8_t> received;
};

Error notLoaded()
{
  return Error{ .code = "not_loaded", .message = "No game is loaded" };
}

std::uint16_t wordAt( std::span<std::uint8_t const> ram, std::size_t at )
{
  return static_cast<std::uint16_t>( ( ram[at] << 8U ) | ram[at + 1] );
}

void putWord( machine::Machine& machine, std::size_t at, std::uint16_t value )
{
  std::array const bytes{ static_cast<std::uint8_t>( value >> 8U ), static_cast<std::uint8_t>( value ) };
  machine.writeWorkRam( at, bytes );
}

std::optional<std::vector<std::uint8_t>> parseHex( std::string const& text )
{
  if ( text.size() % 2 != 0 )
  {
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve( text.size() / 2 );
  for ( std::size_t at = 0; at < text.size(); at += 2 )
  {
    unsigned value = 0;
    for ( char const c : { text[at], text[at + 1] } )
    {
      unsigned digit = 0;
      if ( c >= '0' && c <= '9' )
      {
        digit = static_cast<unsigned>( c - '0' );
      }
      else if ( c >= 'a' && c <= 'f' )
      {
        digit = static_cast<unsigned>( c - 'a' + 10 );
      }
      else if ( c >= 'A' && c <= 'F' )
      {
        digit = static_cast<unsigned>( c - 'A' + 10 );
      }
      else
      {
        return std::nullopt;
      }
      value = ( value << 4U ) | digit;
    }
    bytes.push_back( static_cast<std::uint8_t>( value ) );
  }
  return bytes;
}

std::string toHex( std::span<std::uint8_t const> bytes )
{
  std::string text;
  text.reserve( bytes.size() * 2 );
  for ( std::uint8_t const byte : bytes )
  {
    text += fmt::format( "{:02x}", byte );
  }
  return text;
}

/// Attaches to the block the test ROM has published, if it has: marks it
/// active, as the simulator does, so that the ROM uses it. Answers where the
/// block is.
std::optional<std::size_t> attach( Link& link, machine::Machine& machine )
{
  if ( link.machine != &machine )
  {
    link = Link{ .machine = &machine, .base = std::nullopt, .received = {} };
  }
  if ( link.base )
  {
    return link.base;
  }
  auto const ram = machine.workRam();
  for ( std::size_t at = 0; at + LINK_BLOCK <= ram.size(); at += 2 )
  {
    if ( std::equal( LINK_MAGIC.begin(), LINK_MAGIC.end(), ram.begin() + static_cast<std::ptrdiff_t>( at ) ) &&
         ram[at + LINK_TARGET_READY] == 1 )
    {
      link.base = at;
      std::array const active{ std::uint8_t{ 1 } };
      machine.writeWorkRam( at + LINK_ACTIVE, active );
      return link.base;
    }
  }
  return std::nullopt;
}

/// Moves what the test ROM has sent from its ring into `link.received`.
void drain( Link& link, machine::Machine& machine, std::size_t base )
{
  auto const ram = machine.workRam();
  std::uint16_t const head = wordAt( ram, base + LINK_OUT_HEAD );
  std::uint16_t tail = wordAt( ram, base + LINK_OUT_TAIL );
  if ( tail == head )
  {
    return;
  }
  while ( tail != head )
  {
    link.received.push_back( ram[base + LINK_OUT_BUFFER + ( tail & ( LINK_RING - 1 ) )] );
    ++tail;
  }
  putWord( machine, base + LINK_OUT_TAIL, tail );
}

} // namespace

void addTestMethods( Dispatcher& dispatcher, Emulator& emulator )
{
  auto link = std::make_shared<Link>();

  dispatcher.add(
      "test.status",
      info( "The status block PGMTest's pages publish at the top of work RAM, 0x81F000: a 16-bit magic "
            "naming the page, then words whose meaning is the page's own.",
            { { .name = "words",
                .type = "integer",
                .description = "How many 16-bit words to answer, magic included; 32 if left out.",
                .required = false } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        machine::Machine const* const machine = emulator.machine();
        if ( machine == nullptr )
        {
          return std::unexpected( notLoaded() );
        }
        std::uint64_t count = 32;
        if ( params.contains( "words" ) )
        {
          auto const given = requireUnsigned( params, "words" );
          if ( !given || *given == 0 || *given > STATUS_WORDS )
          {
            return std::unexpected( given ? badRequest( "words is 1 to 2048" ) : given.error() );
          }
          count = *given;
        }
        auto const ram = machine->workRam();
        Json words = Json::array();
        for ( std::uint64_t i = 0; i < count; ++i )
        {
          words.push_back( wordAt( ram, STATUS_AT + static_cast<std::size_t>( i * 2 ) ) );
        }
        std::uint16_t const magic = wordAt( ram, STATUS_AT );
        std::string const name{ static_cast<char>( magic >> 8U ), static_cast<char>( magic & 0xffU ) };
        return Json{ { "address", 0x800000 + STATUS_AT }, { "magic", magic }, { "name", name }, { "words", words } };
      } );

  dispatcher.add( "debug_link.start",
                  info( "Opens PGMTest's debug link, as the simulator does: through the RFIF block the test ROM "
                        "publishes in work RAM. comms_addr, the ROM mailbox's address, is taken and not used." ),
                  [&emulator, link]( Json const& /*params*/ ) -> Outcome
                  {
                    machine::Machine* const machine = emulator.machine();
                    if ( machine == nullptr )
                    {
                      return std::unexpected( notLoaded() );
                    }
                    *link = Link{ .machine = machine, .base = std::nullopt, .received = {} };
                    attach( *link, *machine );
                    return Json::object();
                  } );

  dispatcher.add( "debug_link.stop",
                  info( "Closes the debug link, telling the test ROM so." ),
                  [&emulator, link]( Json const& /*params*/ ) -> Outcome
                  {
                    machine::Machine* const machine = emulator.machine();
                    if ( machine != nullptr && link->machine == machine && link->base )
                    {
                      std::array const inactive{ std::uint8_t{ 0 } };
                      machine->writeWorkRam( *link->base + LINK_ACTIVE, inactive );
                    }
                    *link = Link{};
                    return Json::object();
                  } );

  dispatcher.add(
      "debug_link.write",
      info( "Sends bytes to the test ROM over the debug link, running the machine until the test ROM has published "
            "its block and the bytes fit in its ring.",
            { { .name = "data_hex", .type = "string", .description = "The bytes, as hex." },
              { .name = "timeout_cycles_per_byte",
                .type = "integer",
                .description = "Master ticks to give up after, per byte; 2000000 if left out.",
                .required = false } } ),
      [&emulator, link]( Json const& params ) -> Outcome
      {
        machine::Machine* const machine = emulator.machine();
        auto const text = requireString( params, "data_hex" );
        if ( machine == nullptr || !text )
        {
          return std::unexpected( machine == nullptr ? notLoaded() : text.error() );
        }
        auto const data = parseHex( *text );
        if ( !data )
        {
          return std::unexpected( badRequest( "data_hex is not hex" ) );
        }
        std::uint64_t perByte = DEFAULT_TIMEOUT;
        if ( params.contains( "timeout_cycles_per_byte" ) )
        {
          auto const given = requireUnsigned( params, "timeout_cycles_per_byte" );
          if ( !given )
          {
            return std::unexpected( given.error() );
          }
          perByte = *given;
        }
        auto const timeout = static_cast<std::int64_t>( perByte * std::max<std::size_t>( data->size(), 1 ) );
        std::int64_t elapsed = 0;
        std::optional<std::size_t> attached = attach( *link, *machine );
        while ( !attached )
        {
          if ( elapsed >= timeout )
          {
            return std::unexpected(
                Error{ .code = "debug_link_timeout", .message = "The test ROM published no debug link" } );
          }
          elapsed += step( *machine, ATTACH_STEP );
          attached = attach( *link, *machine );
        }

        std::size_t const base = *attached;
        std::size_t sent = 0;
        while ( sent < data->size() )
        {
          auto const ram = machine->workRam();
          std::uint16_t head = wordAt( ram, base + LINK_IN_HEAD );
          std::uint16_t const tail = wordAt( ram, base + LINK_IN_TAIL );
          std::size_t space = LINK_RING - static_cast<std::uint16_t>( head - tail );
          bool const wrote = space != 0;
          while ( space != 0 && sent < data->size() )
          {
            std::array const byte{ ( *data )[sent] };
            machine->writeWorkRam( base + LINK_IN_BUFFER + ( head & ( LINK_RING - 1 ) ), byte );
            ++head;
            ++sent;
            --space;
          }
          if ( wrote )
          {
            putWord( *machine, base + LINK_IN_HEAD, head );
          }
          if ( sent < data->size() )
          {
            if ( elapsed >= timeout )
            {
              return std::unexpected(
                  Error{ .code = "debug_link_timeout", .message = "The test ROM did not take every byte" } );
            }
            elapsed += step( *machine, RING_STEP );
          }
        }
        return Json::object();
      } );

  dispatcher.add(
      "debug_link.read",
      info( "Receives bytes the test ROM sent over the debug link, running the machine until at least min_bytes "
            "have come or the timeout passes. available is what is left to read.",
            { { .name = "max_bytes", .type = "integer", .description = "The most bytes to answer." },
              { .name = "min_bytes",
                .type = "integer",
                .description = "The fewest to wait for; 0 if left out.",
                .required = false },
              { .name = "timeout_cycles",
                .type = "integer",
                .description = "Master ticks to wait at most; 2000000 if left out.",
                .required = false } } ),
      [&emulator, link]( Json const& params ) -> Outcome
      {
        machine::Machine* const machine = emulator.machine();
        auto const maxBytes = requireUnsigned( params, "max_bytes" );
        if ( machine == nullptr || !maxBytes )
        {
          return std::unexpected( machine == nullptr ? notLoaded() : maxBytes.error() );
        }
        std::uint64_t minBytes = 0;
        std::uint64_t timeout = DEFAULT_TIMEOUT;
        for ( auto [name, target] : { std::pair{ "min_bytes", &minBytes }, std::pair{ "timeout_cycles", &timeout } } )
        {
          if ( params.contains( name ) )
          {
            auto const given = requireUnsigned( params, name );
            if ( !given )
            {
              return std::unexpected( given.error() );
            }
            *target = *given;
          }
        }
        minBytes = std::min( minBytes, *maxBytes );

        std::int64_t elapsed = 0;
        if ( auto const base = attach( *link, *machine ) )
        {
          for ( ;; )
          {
            drain( *link, *machine, *base );
            if ( link->received.size() >= minBytes || std::cmp_greater_equal( elapsed, timeout ) )
            {
              break;
            }
            elapsed += step( *machine, RING_STEP );
          }
        }
        // Where size_t is 32 bits, a browser, the smaller of the two still fits it.
        auto const count = static_cast<std::size_t>( std::min<std::uint64_t>( *maxBytes, link->received.size() ) );
        std::vector<std::uint8_t> bytes( link->received.begin(),
                                         link->received.begin() + static_cast<std::ptrdiff_t>( count ) );
        link->received.erase( link->received.begin(), link->received.begin() + static_cast<std::ptrdiff_t>( count ) );
        return Json{ { "data_hex", toHex( bytes ) }, { "available", link->received.size() } };
      } );
}

} // namespace pgm::control
