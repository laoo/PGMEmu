#include "Z80.hpp"

// z80.h is upstream's, unmodified (libextern/README.md); MSVC finds code its
// switch can never reach, which is upstream's design and no fault here.
#ifdef _MSC_VER
#pragma warning( push )
#pragma warning( disable : 4702 )
#endif
#define CHIPS_IMPL
#include <z80.h>
#ifdef _MSC_VER
#pragma warning( pop )
#endif

namespace pgm::machine
{

struct Z80::State
{
  z80_t cpu{};
  std::uint64_t pins{};
  bool interrupt{};
  bool nmi{};
  /// Stopped at an access for a bus request; resume() makes it.
  bool holding{};
};

namespace
{

/// Whether the pins of a T-state begin an opcode fetch.
bool beginsFetch( std::uint64_t pins )
{
  return ( pins & ( Z80_M1 | Z80_MREQ | Z80_RD ) ) == ( Z80_M1 | Z80_MREQ | Z80_RD );
}

/// Carries out the access the pins describe.
void access( std::uint64_t& pins, Z80Bus& bus )
{
  // z80.h raises RD or WR for one tick per access, with the address and, for
  // a write, the data; a read's data must be on the bus before the next tick.
  std::uint16_t const address = Z80_GET_ADDR( pins );
  if ( ( pins & Z80_MREQ ) != 0 )
  {
    if ( ( pins & Z80_RD ) != 0 )
    {
      Z80_SET_DATA( pins, static_cast<std::uint64_t>( bus.read( address ) ) );
    }
    else if ( ( pins & Z80_WR ) != 0 )
    {
      bus.write( address, Z80_GET_DATA( pins ) );
    }
  }
  else if ( ( pins & Z80_IORQ ) != 0 )
  {
    if ( ( pins & Z80_M1 ) != 0 )
    {
      Z80_SET_DATA( pins, static_cast<std::uint64_t>( bus.acknowledge( address ) ) );
    }
    else if ( ( pins & Z80_RD ) != 0 )
    {
      Z80_SET_DATA( pins, static_cast<std::uint64_t>( bus.in( address ) ) );
    }
    else if ( ( pins & Z80_WR ) != 0 )
    {
      bus.out( address, Z80_GET_DATA( pins ) );
    }
  }
}

} // namespace

Z80::Z80() : mState{ std::make_unique<State>() }
{
  // tv80s is not reset at power-up, as its reset is a latch bit that starts
  // clear, so its registers hold what the simulator starts every flip-flop
  // with: zero.
  mState->pins = z80_init( &mState->cpu );
  setRegisters( Z80Registers{} );
}

Z80::~Z80() = default;

void Z80::reset()
{
  // tv80s resets what tv80_core.v holds in flip-flops, and leaves the
  // register file, BC to IY and their alternates, as it was.
  Z80Registers kept = registers();
  kept.af = 0xffff;
  kept.af2 = 0xffff;
  kept.sp = 0xffff;
  kept.wz = 0;
  kept.pc = 0;
  kept.i = 0;
  kept.r = 0;
  kept.im = 0;
  kept.iff1 = false;
  kept.iff2 = false;
  mState->pins = z80_reset( &mState->cpu );
  mState->holding = false;
  setRegisters( kept );
}

void Z80::setInterrupt( bool asserted )
{
  mState->interrupt = asserted;
}

void Z80::setNmi( bool asserted )
{
  mState->nmi = asserted;
}

bool Z80::tick( Z80Bus& bus, bool busRequested )
{
  State& s = *mState;
  std::uint64_t pins = s.pins & ~( Z80_INT | Z80_NMI );
  pins |= ( s.interrupt ? Z80_INT : 0 ) | ( s.nmi ? Z80_NMI : 0 );
  pins = z80_tick( &s.cpu, pins );
  s.pins = pins;
  if ( busRequested && beginsFetch( pins ) )
  {
    s.holding = true;
    return true;
  }
  access( s.pins, bus );
  return false;
}

void Z80::run(
    Z80Bus& bus, std::span<std::uint8_t, 0x10000> ram, std::int64_t& pulses, std::int64_t until, bool busRequested )
{
  State& s = *mState;
  std::uint64_t pins = s.pins;
  while ( pulses < until )
  {
    ++pulses;
    pins &= ~( Z80_INT | Z80_NMI );
    pins |= ( s.interrupt ? Z80_INT : 0 ) | ( s.nmi ? Z80_NMI : 0 );
    pins = z80_tick( &s.cpu, pins );
    if ( busRequested && beginsFetch( pins ) )
    {
      s.holding = true;
      break;
    }
    if ( ( pins & Z80_MREQ ) != 0 )
    {
      std::uint16_t const address = Z80_GET_ADDR( pins );
      if ( ( pins & Z80_RD ) != 0 )
      {
        Z80_SET_DATA( pins, static_cast<std::uint64_t>( ram[address] ) );
      }
      else if ( ( pins & Z80_WR ) != 0 )
      {
        ram[address] = Z80_GET_DATA( pins );
      }
    }
    else if ( ( pins & Z80_IORQ ) != 0 )
    {
      access( pins, bus );
      s.pins = pins;
      return;
    }
  }
  s.pins = pins;
}

void Z80::resume( Z80Bus& bus )
{
  if ( mState->holding )
  {
    mState->holding = false;
    access( mState->pins, bus );
  }
}

void Z80::serialize( StateWriter& archive )
{
  archive( mState->cpu );
  archive( mState->pins );
  archive( mState->interrupt );
  archive( mState->nmi );
  archive( mState->holding );
}

void Z80::serialize( StateReader& archive )
{
  archive( mState->cpu );
  archive( mState->pins );
  archive( mState->interrupt );
  archive( mState->nmi );
  archive( mState->holding );
}

bool Z80::holding() const
{
  return mState->holding;
}

bool Z80::instructionBoundary() const
{
  return z80_opdone( &mState->cpu );
}

std::uint16_t Z80::address() const
{
  return Z80_GET_ADDR( mState->pins );
}

bool Z80::halted() const
{
  return ( mState->pins & Z80_HALT ) != 0;
}

Z80Registers Z80::registers() const
{
  z80_t const& c = mState->cpu;
  return Z80Registers{ .af = c.af,
                       .bc = c.bc,
                       .de = c.de,
                       .hl = c.hl,
                       .ix = c.ix,
                       .iy = c.iy,
                       .sp = c.sp,
                       .pc = c.pc,
                       .wz = c.wz,
                       .af2 = c.af2,
                       .bc2 = c.bc2,
                       .de2 = c.de2,
                       .hl2 = c.hl2,
                       .i = c.i,
                       .r = c.r,
                       .im = c.im,
                       .iff1 = c.iff1,
                       .iff2 = c.iff2 };
}

void Z80::setRegisters( Z80Registers const& registers )
{
  z80_t& c = mState->cpu;
  c.af = registers.af;
  c.bc = registers.bc;
  c.de = registers.de;
  c.hl = registers.hl;
  c.ix = registers.ix;
  c.iy = registers.iy;
  c.sp = registers.sp;
  c.wz = registers.wz;
  c.af2 = registers.af2;
  c.bc2 = registers.bc2;
  c.de2 = registers.de2;
  c.hl2 = registers.hl2;
  c.i = registers.i;
  c.r = registers.r;
  c.im = registers.im;
  c.iff1 = registers.iff1;
  c.iff2 = registers.iff2;
  mState->pins = z80_prefetch( &c, registers.pc );
}

} // namespace pgm::machine
