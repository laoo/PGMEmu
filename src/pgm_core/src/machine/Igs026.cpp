#include "Igs026.hpp"

#include "ClockEnables.hpp"

#include <algorithm>

namespace pgm::machine
{

namespace
{

constexpr std::size_t SOUND_LATCH_1 = 1;
constexpr std::size_t SOUND_LATCH_2 = 2;
constexpr std::size_t Z80_CONTROL = 4;
constexpr std::size_t Z80_BUS = 5;
constexpr std::size_t SOUND_LATCH_6 = 6;
constexpr std::uint16_t BUS_REQUEST = 0x45d3;
/// Written to 0xC00008, it pulses the ICS2115's reset as well (igs026_x.sv).
constexpr std::uint16_t ICS2115_RESET = 0x5050;

constexpr std::uint32_t RAM_WINDOW = 0x10000;

/// The ce_33m pulse that falls with ce_8m pulse `z80Pulses`: the 2nd, 6th,
/// 10th... (ClockEnables.hpp).
constexpr std::int64_t ce33mPulseOfZ80( std::int64_t z80Pulses )
{
  return ( 4 * z80Pulses ) - 2;
}

/// The first Z80 tick at or after ce_33m pulse `pulses`.
constexpr std::int64_t z80PulseAtOrAfter( std::int64_t pulses )
{
  return pulses == Ics2115::NEVER ? Ics2115::NEVER : ( pulses + 2 + 3 ) / 4;
}

} // namespace

/// The Z80's view of the board: its RAM, and an I/O space decoded by address
/// bits 8 to 11 alone.
class Igs026::Z80Side final : public Z80Bus
{
public:
  explicit Z80Side( Igs026& io ) : mIo{ io } {}

  std::uint8_t read( std::uint16_t address ) override
  {
    return mIo.mZ80Ram.at( address );
  }

  void write( std::uint16_t address, std::uint8_t value ) override
  {
    mIo.mZ80Ram.at( address ) = value;
  }

  std::uint8_t in( std::uint16_t port ) override
  {
    switch ( ( port >> 8U ) & 0xfU )
    {
    case 0:
    {
      mIo.syncIcs2115( mIo.mZ80Pulses );
      std::uint8_t const value = mIo.mIcs2115.read( port & 3U );
      mIo.syncIcs2115( mIo.mZ80Pulses );
      return value;
    }
    case 1:
      return static_cast<std::uint8_t>( mIo.mLatch[SOUND_LATCH_6] );
    case 2:
      // Reading the 68000's command takes its NMI away.
      mIo.mZ80Nmi = false;
      mIo.mZ80.setNmi( false );
      return static_cast<std::uint8_t>( mIo.mLatch[SOUND_LATCH_1] );
    case 4:
      return static_cast<std::uint8_t>( mIo.mLatch[SOUND_LATCH_2] );
    default:
      // Nothing drives the data bus, and the RAM's output reaches it.
      return mIo.mZ80Ram.at( port );
    }
  }

  void out( std::uint16_t port, std::uint8_t value ) override
  {
    auto const setLow = [&]( std::size_t latch )
    { mIo.mLatch.at( latch ) = static_cast<std::uint16_t>( ( mIo.mLatch.at( latch ) & 0xff00U ) | value ); };
    switch ( ( port >> 8U ) & 0xfU )
    {
    case 0:
      mIo.syncIcs2115( mIo.mZ80Pulses );
      mIo.mIcs2115.write( port & 3U, value );
      mIo.syncIcs2115( mIo.mZ80Pulses );
      break;
    case 1:
      setLow( SOUND_LATCH_6 );
      break;
    case 2:
      setLow( SOUND_LATCH_1 );
      break;
    case 4:
      setLow( SOUND_LATCH_2 );
      break;
    default:
      break;
    }
  }

  std::uint8_t acknowledge( std::uint16_t address ) override
  {
    // The board puts whatever its I/O decode gives on the bus. The BIOS's
    // sound driver runs in interrupt mode 1, which ignores it; this answers
    // without the side effects of a read (docs/hardware/differences.md).
    switch ( ( address >> 8U ) & 0xfU )
    {
    case 0:
      return 0xff;
    case 1:
      return static_cast<std::uint8_t>( mIo.mLatch[SOUND_LATCH_6] );
    case 2:
      return static_cast<std::uint8_t>( mIo.mLatch[SOUND_LATCH_1] );
    case 4:
      return static_cast<std::uint8_t>( mIo.mLatch[SOUND_LATCH_2] );
    default:
      return mIo.mZ80Ram.at( address );
    }
  }

private:
  Igs026& mIo;
};

Igs026::Igs026( Z80& z80, Ics2115& ics2115 ) : mZ80{ z80 }, mIcs2115{ ics2115 } {}

void Igs026::reset( Time now )
{
  advanceTo( now );
  mLatch = {};
  mZ80Nmi = false;
  mZ80.setNmi( false );
  mIcs2115.reset();
  releaseBus();
  syncIcs2115( mZ80Pulses );
}

bool Igs026::z80InReset() const
{
  return ( mLatch[Z80_CONTROL] & 1U ) != 0;
}

bool Igs026::busRequested() const
{
  return mLatch[Z80_BUS] == BUS_REQUEST;
}

bool Igs026::busGranted() const
{
  return busRequested() && ( z80InReset() || mZ80.holding() );
}

void Igs026::releaseBus()
{
  // The fetch's first T-state ran before the Z80 stopped, and the next runs
  // on the next tick. tv80s stops at the end of any machine cycle, and starts
  // the next two ticks after the bus comes back; modelling that makes
  // orlegend's sound drift from the RTL simulation's
  // (docs/hardware/differences.md).
  if ( mZ80.holding() )
  {
    Z80Side side{ *this };
    mZ80.resume( side );
  }
}

void Igs026::syncIcs2115( std::int64_t z80Pulses )
{
  mIcs2115.advanceTo( ce33mPulseOfZ80( z80Pulses ) );
  mZ80.setInterrupt( mIcs2115.irq() );
  mIcs2115DueAt = z80PulseAtOrAfter( mIcs2115.nextEvent() );
}

void Igs026::advanceTo( Time now )
{
  std::int64_t const ticks = masterTicks( now );
  std::int64_t const target = ce8mPulses( ticks );
  Z80Side side{ *this };
  while ( mZ80Pulses < target )
  {
    if ( z80InReset() || mZ80.holding() )
    {
      mZ80Pulses = target;
      break;
    }
    if ( mZ80Pulses + 1 >= mIcs2115DueAt )
    {
      syncIcs2115( mZ80Pulses + 1 );
    }
    if ( mBreakpoints.empty() )
    {
      // As many T-states as fall before the ICS2115 is next due, in one go.
      // An I/O access ends the run early, as it may have moved that.
      std::int64_t const limit = mIcs2115DueAt == Ics2115::NEVER ? target : std::min( target, mIcs2115DueAt - 1 );
      mZ80.run( side, mZ80Ram, mZ80Pulses, std::max( limit, mZ80Pulses + 1 ), busRequested() );
      continue;
    }
    ++mZ80Pulses;
    mZ80.tick( side, busRequested() );
    if ( !mBreakpointHit && mZ80.instructionBoundary() && mBreakpoints.contains( mZ80.address() ) )
    {
      // The registers' pc has already moved past the opcode being fetched.
      mBreakpointHit = mZ80.registers();
      mBreakpointHit->pc = mZ80.address();
    }
  }
  // The ICS2115 runs on to the present: no Z80 tick falls before it.
  mIcs2115.advanceTo( ce33mPulses( ticks ) );
  mZ80.setInterrupt( mIcs2115.irq() );
  mIcs2115DueAt = z80PulseAtOrAfter( mIcs2115.nextEvent() );
}

std::uint16_t Igs026::read( Time now, std::uint32_t address, bool upper, bool lower )
{
  advanceTo( now );
  if ( ( address & RAM_WINDOW ) != 0 )
  {
    if ( !busGranted() )
    {
      return 0;
    }
    std::size_t const at = address & 0xfffeU;
    return static_cast<std::uint16_t>( ( mZ80Ram[at] << 8U ) | mZ80Ram[at + 1] );
  }

  switch ( address & 0xeU )
  {
  case 0x2:
    return mLatch[1];
  case 0x4:
    return mLatch[2];
  case 0x6:
    // Any cycle to the RTC's address clocks its port, whatever the strobes.
    return mRtc.access( now, false, false ) ? 1 : 0;
  case 0x8:
    return mLatch[4];
  case 0xa:
    return mLatch[5];
  case 0xc:
    return mLatch[6];
  default:
    (void)upper;
    (void)lower;
    return 0;
  }
}

void Igs026::write( Time now, std::uint32_t address, std::uint16_t value, bool upper, bool lower )
{
  advanceTo( now );
  if ( ( address & RAM_WINDOW ) != 0 )
  {
    if ( !busGranted() )
    {
      return;
    }
    std::size_t const at = address & 0xfffeU;
    if ( upper )
    {
      mZ80Ram[at] = static_cast<std::uint8_t>( value >> 8U );
    }
    if ( lower )
    {
      mZ80Ram[at + 1] = static_cast<std::uint8_t>( value );
    }
    return;
  }

  std::size_t const reg = address & 0xeU;
  if ( reg == 0x6 )
  {
    mRtc.access( now, true, ( value & 1U ) != 0 );
    return;
  }

  // The registers take a write only when the lower strobe is active, and then
  // take the whole data bus: a byte write to an even address is lost.
  if ( !lower )
  {
    return;
  }
  bool const wasInReset = z80InReset();
  switch ( reg )
  {
  case 0x2:
    mLatch[SOUND_LATCH_1] = value;
    mZ80Nmi = true;
    mZ80.setNmi( true );
    break;
  case 0x8:
    mLatch[Z80_CONTROL] = value;
    if ( value == ICS2115_RESET )
    {
      mIcs2115.reset();
      syncIcs2115( mZ80Pulses );
    }
    break;
  case 0x4:
  case 0xa:
  case 0xc:
    mLatch[reg / 2] = value;
    break;
  default:
    break;
  }

  if ( !wasInReset && z80InReset() )
  {
    // The Z80 is held in reset from here, and drops the access it stopped
    // at; it starts again from address 0 when it is let go.
    mZ80.reset();
  }
  else if ( !busRequested() )
  {
    releaseBus();
  }
}

std::span<std::uint8_t const> Igs026::z80Ram() const
{
  return mZ80Ram;
}

std::span<std::uint8_t> Igs026::z80RamForDebugger()
{
  return mZ80Ram;
}

std::set<std::uint32_t>& Igs026::breakpoints()
{
  return mBreakpoints;
}

std::set<std::uint32_t> const& Igs026::breakpoints() const
{
  return mBreakpoints;
}

std::optional<Z80Registers> const& Igs026::breakpointHit() const
{
  return mBreakpointHit;
}

void Igs026::clearBreakpointHit()
{
  mBreakpointHit.reset();
}

} // namespace pgm::machine
